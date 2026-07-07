#include "compiler/sema/passes/type_resolver.hh"

#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/format.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/traits.hh"
#include "compiler/ast/type.hh"
#include "compiler/ast/visitor.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/generic.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/token_type.hh"
#include "support/diagnostic.hh"

namespace ghoti::sema {

auto type_resolver::resolve_types(mod::module& module, context& ctx) -> mod::module_state {
    PROFILE_FUNCTION();

    // Poisoned collection should flush the diagnostics
    const auto poisoned_collection{module.state == mod::module_state::POISONED_SYMBOL_COLLECTION};
    if (poisoned_collection) { module.print_diagnostics(ctx.error_stream); }

    if (module.is_resolvable()) {
        module.state = poisoned_collection ? mod::module_state::POISONED_TYPE_RESOLVING
                                           : mod::module_state::TYPE_RESOLVING;
        ctx.inject_prelude();

        type_resolver resolver{module, ctx};
        for (const auto& node : module.ast) { resolver.resolve(node); }

        if (!ctx.diags.empty() || module.is_poisoned()) {
            return module.error_out(std::move(ctx.diags),
                                    mod::module_state::POISONED_TYPE_RESOLVED);
        }
        module.state = mod::module_state::TYPE_RESOLVED;
    }
    return module.state;
}

// Performs the resolution and poison check & bubble & return operation
#define TRY_RESOLVE(resolvable_expr)                                                       \
    do {                                                                                   \
        resolve(resolvable_expr);                                                          \
        if (last_type_->is_poison()) { return resolving_.set_sema_type(id, *last_type_); } \
    } while (false)

namespace {

[[nodiscard]] auto incomplete_array_item(const source_location& location) -> diagnostic {
    return diagnostic{
        "Array elements cannot have an incomplete type", error::CYCLIC_DEPENDENCY, location};
}

} // namespace

auto type_resolver::visit(ast::node_id id, const ast::array_expr& array) -> void {
    PROFILE_FUNCTION();
    for (const auto& item : array.items) { resolve(item); }
    resolve(array.item_explicit_type);
    auto& item_type{*last_type_.take()};

    if (!item_type.is_resolved()) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            incomplete_array_item(resolving_.ast.location_of(array.item_explicit_type))));
    }

    last_type_.emplace(
        ctx_.get_array(types::mut::CONSTANT, array.null_terminated, array.items.size(), item_type));
    resolving_.set_sema_type(id, *last_type_);
}

template <ast::IndexableID ID>
[[nodiscard]] auto type_resolver::resolve_builtin_call(ID                             id,
                                                       const ast::call_expr&          call,
                                                       const types::builtin_function& builtin)
    -> stdx::result<void, diagnostic> {
    ASSERT(call.function.is<ast::identifier_expr>(), "Builtin function must be a raw ident");
    const auto& params{builtin.params};
    if (call.arguments.size() != params.size()) {
        return make_sema_err(fmt::format("Builtin expects {} arguments, found {}",
                                         params.size(),
                                         call.arguments.size()),
                             error::ARITY_MISMATCH,
                             resolving_.ast.location_of(call.function));
    }

    // There must be an actual builtin present with a token id
    const auto builtin_id{call.function->get_token_type()};
    ASSERT(syntax::get_builtin_opt(builtin_id), "Cannot resolve non-builtin function");

    using syntax::token_type_t;
    gsl::not_null<type*> return_type = &ctx_.get_poison();

    // Indexing can be done freely as arity is already validated
    switch (builtin_id) {
    case token_type_t::BUILTIN_ALIGN_CAST:
    case token_type_t::BUILTIN_PTR_CAST:
    case token_type_t::BUILTIN_BIT_CAST:
    case token_type_t::BUILTIN_AS:         {
        // These builtins take in a resulting type to cast to
        return_type = get_resolved_call_arg_type(call.arguments[0]);
        break;
    }
    case token_type_t::BUILTIN_CONST_CAST: {
        // Pointer/reference is checked since it is an invariant of the cast
        auto& expr_type{*get_resolved_call_arg_type(call.arguments[0])};
        if (expr_type.get_kind() == type_kind::POINTER ||
            expr_type.get_kind() == type_kind::REFERENCE) {
            return_type = ctx_.pool.strip_const(expr_type);
        } else {
            return make_sema_err(fmt::format("Expected pointer or reference type; found '{}'",
                                             type_kind_display_name(expr_type.get_kind())),
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(call.arguments[0]));
        }
        break;
    }
    case token_type_t::BUILTIN_VOLATILE_CAST: {
        auto& expr_type{*get_resolved_call_arg_type(call.arguments[0])};
        return_type = ctx_.pool.strip_volatile(expr_type);
        break;
    }
    case token_type_t::BUILTIN_INT_FROM_PTR:
    case token_type_t::BUILTIN_ALIGN_OF:
    case token_type_t::BUILTIN_SIZE_OF:
    case token_type_t::BUILTIN_CLZ:
    case token_type_t::BUILTIN_CTZ:
    case token_type_t::BUILTIN_POP_COUNT:    {
        ASSERT(builtin.return_type.get_kind() == type_kind::USIZE);
        return_type = &builtin.return_type;
        break;
    }
    // @typeOf returns a type as per documentation, but it's not the literal `type` type
    case token_type_t::BUILTIN_TYPE_OF: {
        ASSERT(builtin.return_type.get_kind() == type_kind::TYPE);
        auto&       instance_type{*get_resolved_call_arg_type(call.arguments[0])};
        const auto& instance_data{instance_type.get_data()};
        if (instance_data.is<types::deferred_call>() || instance_data.is<types::deferred_array>()) {
            return_type = ctx_.pool[{type_kind::TYPE, types::mut::CONSTANT, &call}];
            return_type->resolve_if<types::deferred_call>(call);
        } else {
            return_type = ctx_.pool[{type_kind::TYPE, types::mut::CONSTANT, instance_type}];
            return_type->resolve_if<types::meta_type>(instance_type);
        }
        break;
    }
    // @this returns a type as per docs, but it's really a structural type with full determinism
    case token_type_t::BUILTIN_THIS:
        if (const auto user_type{user_type_stack_.peek()}) {
            return_type = user_type.get();
            break;
        }

        return make_sema_err("@this() may only be used inside of structs, unions, and enums",
                             error::TYPE_MISMATCH,
                             resolving_.ast.location_of(id));
    case token_type_t::BUILTIN_PTR_FROM_ARRAY: {
        auto& array_type{*get_resolved_call_arg_type(call.arguments[0])};
        auto& type_data{array_type.get_data()};
        // The new type uses a new key to align with normal pointer creation
        if (const auto array_data{type_data.as_opt<types::array>()}) {
            return_type = &ctx_.get_pointer(types::mut::CONSTANT, array_data->underlying);
        } else if (const auto deferred_data{type_data.as_opt<types::deferred_array>()}) {
            return_type = &ctx_.get_pointer(types::mut::CONSTANT, deferred_data->underlying);
        } else {
            return make_sema_err(fmt::format("Expected an array-yielding expression; found '{}'",
                                             type_kind_display_name(array_type.get_kind())),
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(call.arguments[0]));
        }
        break;
    }
    case token_type_t::BUILTIN_PTR_FROM_INT: {
        auto& requested_output{*get_resolved_call_arg_type(call.arguments[0])};
        if (requested_output.get_kind() == type_kind::POINTER) {
            return_type = &requested_output;
            break;
        }
        return make_sema_err(fmt::format("Expected a pointer type; found '{}'",
                                         type_kind_display_name(requested_output.get_kind())),
                             error::TYPE_MISMATCH,
                             get_call_arg_location(call.arguments[0]));
    }
    case token_type_t::BUILTIN_SLICE_FROM_PTR: {
        auto& ptr_type{*get_resolved_call_arg_type(call.arguments[0])};
        if (const auto ptr_data{ptr_type.get_data().as_opt<types::pointer>()}) {
            // The resulting slice isn't null terminated since the pointer gives no guarantee
            return_type = &ctx_.get_slice(types::mut::CONSTANT, false, ptr_data->underlying);
            break;
        }

        return make_sema_err(fmt::format("Expected a pointer-yielding expression; found '{}'",
                                         type_kind_display_name(ptr_type.get_kind())),
                             error::TYPE_MISMATCH,
                             get_call_arg_location(call.arguments[0]));
    }
    case token_type_t::BUILTIN_TAG_NAME: {
        ASSERT(builtin.return_type.get_kind() == type_kind::SLICE);
        return_type = &builtin.return_type;
        break;
    }
    case token_type_t::BUILTIN_MEMCPY:
    case token_type_t::BUILTIN_MEMSET:
    case token_type_t::BUILTIN_MEMMOVE: {
        ASSERT(builtin.return_type.get_kind() == type_kind::VOID);
        return_type = &builtin.return_type;
        break;
    }
    // Many of these return @typeOf(expression) which is trivial
    case token_type_t::BUILTIN_MUL_ADD:
    case token_type_t::BUILTIN_SQRT:
    case token_type_t::BUILTIN_SIN:
    case token_type_t::BUILTIN_COS:
    case token_type_t::BUILTIN_TAN:
    case token_type_t::BUILTIN_EXP:
    case token_type_t::BUILTIN_EXP2:
    case token_type_t::BUILTIN_LOG:
    case token_type_t::BUILTIN_LOG2:
    case token_type_t::BUILTIN_LOG10:
    case token_type_t::BUILTIN_ABS:
    case token_type_t::BUILTIN_FLOOR:
    case token_type_t::BUILTIN_CEIL:    {
        return_type = get_resolved_call_arg_type(call.arguments[0]);
        break;
    }
    case token_type_t::BUILTIN_PANIC: {
        ASSERT(builtin.return_type.get_kind() == type_kind::NORETURN);
        return_type = &builtin.return_type;
        break;
    }
    default: UNREACHABLE("Unimplemented builtin type resolution");
    }

    resolving_.set_sema_type(id, *return_type);
    last_type_.emplace(*return_type);
    return {};
}

template auto type_resolver::resolve_builtin_call<ast::node_id>(ast::node_id,
                                                                const ast::call_expr&,
                                                                const types::builtin_function&)
    -> stdx::result<void, diagnostic>;
template auto type_resolver::resolve_builtin_call<ast::explicit_type_id>(
    ast::explicit_type_id, const ast::call_expr&, const types::builtin_function&)
    -> stdx::result<void, diagnostic>;

auto type_resolver::resolve_call_args(gsl::span<const ast::call_expr::argument> args)
    -> resolve_result {
    bool any_poison{false};
    for (const auto& arg : args) {
        any_poison |= arg.visit([this](auto id) -> bool {
            resolve(id);
            return last_type_.take()->is_poison();
        });
    }
    return any_poison ? resolve_result::POISONED : resolve_result::OK;
}

auto type_resolver::get_resolved_call_arg_type(const ast::call_expr::argument& arg)
    -> gsl::not_null<type*> {
    return &arg.visit(
        [this](ast::expr_handle id) -> auto& {
            // Labels store their actual type in nested node data
            if (const auto label{resolving_.ast.get_as_opt<ast::label_expr>(id)}) {
                auto type{resolving_.get_sema_type_opt(label->name)};
                ASSERT(type, "Labeled call arg was not typed");
                return *type;
            }

            auto type{resolving_.get_sema_type_opt(id)};
            ASSERT(type, "Call argument was not typed");
            return *type;
        },
        [this](ast::explicit_type_id id) -> auto& {
            auto type{resolving_.get_sema_type_opt(id)};
            ASSERT(type, "Call argument was not typed");
            return *type;
        });
}

auto type_resolver::get_call_arg_location(const ast::call_expr::argument& arg) -> source_location {
    return arg.visit([this](auto id) -> source_location { return resolving_.ast.location_of(id); });
}

namespace {

[[nodiscard]] auto any_param_generic(auto&& params) noexcept -> bool {
    return std::ranges::contains(params, type_kind::AUTO, &type::get_kind);
}

} // namespace

template <ast::IndexableID ID>
auto type_resolver::resolve_call(ID id, const ast::call_expr& call) -> void {
    // The call can only yield a non-poison type if the function is valid
    resolve(call.function);
    auto& callee_type{*last_type_.take()};
    if (callee_type.is_poison()) {
        resolve_call_args(call.arguments);
        return last_type_.emplace(ctx_.poison_node(resolving_, id));
    }
    resolving_.set_sema_type(call.function, callee_type);

    // Verify that the type in the function is callable and store the return type
    auto& callee_data{callee_type.get_data()};
    if (const auto function_type{callee_data.as_opt<types::function>()}) {
        const auto has_implicit_self{function_type->has_self &&
                                     resolving_.ast.get_as_opt<ast::dot_expr>(call.function)};

        // Check the arity of the function against params before resetting last type
        const auto& params{function_type->params};
        const usize param_offset{has_implicit_self ? 1UZ : 0UZ};
        const auto  expected_arity{params.size() - param_offset};
        if (call.arguments.size() != expected_arity) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format(
                    "Expected {} arguments, found {}", expected_arity, call.arguments.size()),
                error::ARITY_MISMATCH,
                resolving_.ast.location_of(call.function)));
        }

        if (any_param_generic(params)) {
            auto concrete_arg_types{ctx_.pool.get_many_unsafe(call.arguments.size())};
            bool any_arg_poison{false};
            for (usize i{0}; auto [param_type, arg] :
                             std::views::zip(params.subspan(param_offset), call.arguments)) {
                stdx::option<structural_guard> g;
                if (param_type->get_kind() != type_kind::AUTO) {
                    g.emplace(implicit_type_stack_, *param_type);
                }

                auto result_arg_type = arg.visit([this](auto arg_id) -> stdx::option<type&> {
                    resolve(arg_id);
                    auto* arg_type{last_type_.take()};
                    if (arg_type->is_poison()) { return stdx::none; }
                    return arg_type;
                });
                if (!result_arg_type) { any_arg_poison = true; }
                concrete_arg_types[i++] = result_arg_type.take();
            }
            if (any_arg_poison) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }

            generic_instantiation_key key{
                .generic_fn_type = &callee_type,
                .arg_types       = concrete_arg_types,
            };

            if (const auto cached_return_type{ctx_.instantiation_cache.find(key)}) {
                resolving_.set_sema_type(id, *cached_return_type);
                return last_type_.emplace(*cached_return_type);
            }

            const auto fn_info_opt{ctx_.generic_functions.get_opt(callee_type)};
            if (!fn_info_opt) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }

            auto inst_res{instantiate_generic(callee_type, *fn_info_opt, concrete_arg_types)};
            if (!inst_res) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }

            auto& return_type{*inst_res.value()};
            ctx_.instantiation_cache.insert(std::move(key), return_type);
            resolving_.set_sema_type(id, return_type);
            return last_type_.emplace(return_type);
        }

        bool any_arg_poison{false};
        for (auto [param_type, arg] :
             std::views::zip(function_type->params.subspan(param_offset), call.arguments)) {
            const structural_guard g{implicit_type_stack_, *param_type};
            any_arg_poison |= arg.visit([this](auto arg_id) -> bool {
                resolve(arg_id);
                return last_type_.take()->is_poison();
            });
        }
        if (any_arg_poison) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }

        // Functions that return a type cannot be resolved until the constant evaluator
        if (function_type->return_type.get_kind() == type_kind::TYPE) {
            auto& deferred_type{*ctx_.pool[{type_kind::TYPE, types::mut::CONSTANT, &call}]};
            deferred_type.resolve_if<types::deferred_call>(call);

            resolving_.set_sema_type(id, deferred_type);
            return last_type_.emplace(deferred_type);
        }

        // Only arity is checked since the type checker will handle the rest
        resolving_.set_sema_type(id, function_type->return_type);
        last_type_.emplace(function_type->return_type);
    } else if (const auto builtin_type{callee_data.as_opt<types::builtin_function>()}) {
        // There's no need to check any further if the arguments are poisoned
        if (resolve_call_args(call.arguments) == resolve_result::POISONED) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id));
        }

        // Poison the call if there's an error early
        auto result{resolve_builtin_call(id, call, *builtin_type)};
        if (!result) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(result.error())));
        }
    } else {
        return last_type_.emplace(ctx_.poison_node(resolving_,
                                                   id,
                                                   "Expression is not callable",
                                                   error::NON_CALLABLE_EXPRESSION,
                                                   resolving_.ast.location_of(call.function)));
    }
}

VISITOR_TEMPLATE_INIT(type_resolver, resolve_call, const ast::call_expr&)

auto type_resolver::visit(ast::node_id id, const ast::call_expr& call) -> void {
    PROFILE_FUNCTION();
    resolve_call(id, call);
}

auto type_resolver::visit(ast::node_id id, const ast::do_while_loop_expr& do_while) -> void {
    PROFILE_FUNCTION();

    // The loop itself holds the block index, not the block
    auto& loop_type{resolving_.get_sema_type(id)};
    {
        const scope s{table_stack_, loop_type.get_symbol_table_idx(), table_idx_};
        const auto& block{resolving_.ast.get_as<ast::block_stmt>(do_while.block)};
        for (const auto& stmt : block) { TRY_RESOLVE(stmt); }
    }

    TRY_RESOLVE(do_while.condition);
    last_type_.emplace(loop_type);
}

auto type_resolver::resolve_members(gsl::span<type*>                    buf,
                                    gsl::span<const ast::member_handle> members)
    -> stdx::option<gsl::span<type*>> {
    // Only poison the enum once all members are collected
    for (usize i{0}; const auto& member : members) {
        // The resolved statement type is in last_type_ unlike in the symbol collector
        resolve(*member);
        auto& member_type{resolving_.get_sema_type(member)};
        if (member_type.is_poison()) { return stdx::none; }
        buf[i++] = &member_type;
    }
    return buf;
}

template <ast::IndexableID ID>
auto type_resolver::visit(ID id, const ast::enum_expr& enum_expr) -> void {
    PROFILE_FUNCTION();
    if (enum_expr.underlying) { resolve(*enum_expr.underlying); }

    auto&                  enum_type{resolving_.get_sema_type(id)};
    const scope            s{table_stack_, enum_type.get_symbol_table_idx(), table_idx_};
    const structural_guard g{user_type_stack_, enum_type};

    // The underlying type defaults to an i32 as it would in C or C++
    auto& underlying_type{enum_expr.underlying ? resolving_.get_sema_type(*enum_expr.underlying)
                                               : ctx_.get_builtin_resolved_type(type_kind::I32)};

    for (const auto& [name, value] : enum_expr.enumerations) {
        if (value) { TRY_RESOLVE(*value); }

        // The value's type doesn't contribute to the actual enumeration type
        const auto& ident{resolving_.ast.get_as<ast::identifier_expr>(name)};
        auto        sym{ctx_.registry.get_from_opt(table_idx_, ident.name)};
        if (!sym) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }
        resolving_.set_sema_type(name, underlying_type);
        sym->set_kind(symbol_kind::VALUE);
        sym->set_status(symbol_status::RESOLVED);
    }

    auto member_types{ctx_.pool.get_many_unsafe(enum_expr.members.size())};
    committable_resolution<types::enum_t> resolution{enum_type,
                                                     enum_expr.enumerations,
                                                     enum_expr.non_exhaustive,
                                                     underlying_type,
                                                     member_types,
                                                     resolving_};
    if (!resolve_members(member_types, enum_expr.members)) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id));
    }

    resolution.commit();
    last_type_.emplace(enum_type);
}

VISITOR_TEMPLATE_INIT(type_resolver, visit, const ast::enum_expr&)

auto type_resolver::visit(ast::node_id id, const ast::for_loop_expr& for_expr) -> void {
    PROFILE_FUNCTION();
    ASSERT(for_expr.iterables.size() == for_expr.captures.size());

    // The loop itself holds the block index which houses captures, not the block
    auto& loop_type{resolving_.get_sema_type(id)};
    {
        const scope s{table_stack_, loop_type.get_symbol_table_idx(), table_idx_};

        // The captures must be paired with the iterables inner types (shallow type check)
        for (const auto& [capture, iterable] :
             std::views::zip(for_expr.captures, for_expr.iterables)) {
            TRY_RESOLVE(iterable);
            auto& iterable_type{*last_type_.take()};
            resolving_.set_sema_type(iterable, iterable_type);

            // Assign types unconditionally since ignoring discards saves no space
            auto& iterable_data{iterable_type.get_data()};
            if (const auto array{iterable_data.as_opt<types::array>()}) {
                resolving_.set_sema_type(capture.payload, array->underlying);
            } else if (const auto deferred{iterable_data.as_opt<types::deferred_array>()}) {
                resolving_.set_sema_type(capture.payload, deferred->underlying);
            } else if (const auto slice{iterable_data.as_opt<types::slice>()}) {
                resolving_.set_sema_type(capture.payload, slice->underlying);
            } else {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    fmt::format("Iterables may only be arrays or slices; found '{}'",
                                type_kind_display_name(iterable_type.get_kind())),
                    error::TYPE_MISMATCH,
                    resolving_.ast.location_of(iterable)));
            }

            if (capture.payload.is<ast::identifier_expr>()) {
                resolve_symbol_info(capture.payload, symbol_kind::VALUE);
            }
        }
        const auto& block{resolving_.ast.get_as<ast::block_stmt>(for_expr.block)};
        for (const auto& stmt : block) { TRY_RESOLVE(stmt); }
    }

    if (for_expr.non_break) { TRY_RESOLVE(*for_expr.non_break); }
    last_type_.emplace(loop_type);
}

namespace {

[[nodiscard]] auto mutability_from_type_modifier(ast::type_modifier modifier) noexcept
    -> stdx::option<types::mutability_modifiers> {
    using modifier_t = ast::type_modifier::modifier;
    switch (modifier.get_raw()) {
    case modifier_t::VALUE:        return stdx::none;
    case modifier_t::REF:          return types::mut::CONSTANT;
    case modifier_t::MUT_REF:      return types::mut::MUTABLE;
    case modifier_t::PTR:          return types::mut::CONSTANT;
    case modifier_t::MUT_PTR:      return types::mut::MUTABLE;
    case modifier_t::VOLATILE:     return types::mut::CONSTANT_VOLATILE;
    case modifier_t::MUT_VOLATILE: return types::mut::VOLATILE;
    }
}

} // namespace

auto type_resolver::visit(ast::node_id id, const ast::function_expr& fn) -> void {
    PROFILE_FUNCTION();

    // The entire function lives inside of its preallocated scope
    auto&       fn_type{resolving_.get_sema_type(id)};
    const scope s{table_stack_, fn_type.get_symbol_table_idx(), table_idx_};

    const auto true_param_size{fn.parameters.size() + (fn.self ? 1 : 0)};
    auto       param_types{ctx_.pool.get_many_unsafe(true_param_size)};
    usize      param_idx{0};

    if (fn.self) {
        // If self is valid here, then follow a similar tune to the type resolvers
        if (const auto user_type{user_type_stack_.peek()}) {
            const auto modifier{fn.self->modifier};
            const auto mutability{mutability_from_type_modifier(modifier)};

            if (user_type->is_poison()) {
                return last_type_.emplace(ctx_.poison_node(resolving_, id));
            }

            if (!mutability) {
                last_type_.emplace(*user_type);
                resolving_.set_sema_type(fn.self->name, *last_type_);
            } else {
                // Imprint generally here since a new type is always created (else unreachable)
                auto new_key{user_type->get_key()};
                new_key.set_mut(*mutability);
                new_key.imprint(*user_type);

                if (modifier.is_ptr()) {
                    new_key.set_kind(type_kind::POINTER);
                    last_type_.emplace(ctx_.pool[new_key]);
                    last_type_->resolve_if<types::pointer>(*user_type);
                    resolving_.set_sema_type(fn.self->name, *last_type_);
                } else if (modifier.is_ref()) {
                    new_key.set_kind(type_kind::REFERENCE);
                    last_type_.emplace(ctx_.pool[new_key]);
                    last_type_->resolve_if<types::reference>(*user_type);
                    resolving_.set_sema_type(fn.self->name, *last_type_);
                } else {
                    UNREACHABLE("Self parameter has a modifier that slipped through the parser");
                }
            }
        } else {
            resolve_symbol_info(fn.self->name, symbol_kind::POISONED);
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 "Self parameters may only be used inside member functions",
                                 error::ILLEGAL_SELF_PARAMETER,
                                 resolving_.ast.location_of(fn.self->name)));
        }

        param_types[param_idx++] = last_type_.take();
        resolve_symbol_info(fn.self->name, symbol_kind::VALUE);
    }

    // Every parameter contributes to the resolution but not the type key due to unique idx
    for (const auto& param : fn.parameters) {
        TRY_RESOLVE(param.explicit_type);

        auto& param_type{*last_type_.take()};
        param_types[param_idx++] = &param_type;
        resolving_.set_sema_type(param.name, param_type);
        resolve_symbol_info(param.name, symbol_kind::VALUE);
    }

    TRY_RESOLVE(fn.explicit_return_type);
    auto& return_type{*last_type_.take()};
    ASSERT(!fn_type.is_resolved(), "Valued function must not be resolved");

    if (any_param_generic(param_types)) {
        fn_type.resolve<types::function>(fn.self.has_value(), param_types, return_type);
        ctx_.generic_functions.register_function(fn_type, resolving_, id, fn);
        return last_type_.emplace(fn_type);
    }

    const auto is_auto_return{return_type.get_kind() == type_kind::AUTO};
    return_trackers_.emplace_back(
        return_tracker{.return_types = {}, .is_auto_return = is_auto_return});

    const auto& block{resolving_.ast.get_as<ast::block_stmt>(fn.body)};
    for (const auto& stmt : block) { TRY_RESOLVE(stmt); }

    auto tracker{std::move(return_trackers_.back())};
    return_trackers_.pop_back();

    auto& deduced_return_type{is_auto_return ? tracker.deduced_return_type(ctx_) : return_type};
    fn_type.resolve<types::function>(fn.self.has_value(), param_types, deduced_return_type);
    if (is_auto_return) { resolving_.set_sema_type(fn.explicit_return_type, deduced_return_type); }
    last_type_.emplace(fn_type);
}

auto type_resolver::resolve_symbol_info(ast::identifier_handle    handle,
                                        stdx::option<symbol_kind> kind) -> stdx::option<symbol&> {
    const auto& ident{resolving_.ast.get_as<ast::identifier_expr>(handle)};
    return ctx_.registry.get_from_opt(table_idx_, ident.name).transform([&](symbol& sym) -> auto& {
        if (kind) { sym.set_kind(*kind); }
        sym.set_status(symbol_status::RESOLVED);
        return sym;
    });
}

namespace {

template <typename T>
concept HasName = requires(T t) { t.name; };

template <typename T>
concept HasType = requires(T t) { t.explicit_type; };

template <typename T>
concept HasNameOnly = HasName<T> && !HasType<T>;

template <typename T>
concept HasBothNameAndType = HasName<T> && HasType<T>;

} // namespace

namespace {

// Forwards an incomplete aggregate type to safely break cycles if possible
[[nodiscard]] auto forward_type(const mod::module&               target_mod,
                                stdx::option<ast::type_modifier> mod,
                                symbol& sym) noexcept -> stdx::option<type&> {
    const auto node{sym.get_data().as_opt<symbols::node_t>()};
    if (!node) { return stdx::none; }

    if (const auto decl{target_mod.ast.get_as_opt<ast::decl_stmt>(*node)}) {
        if (!decl->value) { return stdx::none; }

        const auto is_aggregate{decl->value->is<ast::struct_expr>() ||
                                decl->value->is<ast::union_expr>() ||
                                decl->value->is<ast::enum_expr>()};
        if ((!mod || (!mod->is_ptr() && !mod->is_ref())) && !is_aggregate) { return stdx::none; }
        return target_mod.get_sema_type_opt(*decl->value);
    }

    if (const auto alias{target_mod.ast.get_as_opt<ast::using_stmt>(*node)}) {
        return target_mod.get_sema_type_opt(alias->explicit_type);
    }
    return stdx::none;
}

} // namespace

template <ast::IndexableID ID> auto type_resolver::resolve_symbol(ID id, symbol& sym) -> void {
    auto& symbol_data{sym.get_data()};
    switch (sym.get_status()) {
    case symbol_status::RESOLVED:
        // Identifier handles are not unique in the tree, but their symbol can be used to find root
        resolving_.set_sema_type(
            id,
            symbol_data.visit(
                [](symbols::builtin& builtin) -> type& { return builtin.get_type(); },
                [this](symbols::label& label) -> type& {
                    const auto defn{label.get_definition()};
                    ASSERT(resolving_.has_sema_type(defn), "Resolved node has no type");
                    return resolving_.get_sema_type(defn);
                },
                [this](auto& sym) -> type& {
                    ASSERT(resolving_.has_sema_type(sym),
                           "Directly indexable symbol was never typed");
                    return resolving_.get_sema_type(sym);
                },
                [this](HasBothNameAndType auto& sym) -> type& {
                    ASSERT(resolving_.has_sema_type(sym.name), "Symbol was never typed");
                    auto& type{resolving_.get_sema_type(sym.name)};
                    ASSERT(type == resolving_.get_sema_type_opt(sym.explicit_type) ||
                               resolving_.get_sema_type_opt(sym.explicit_type)->get_kind() ==
                                   type_kind::AUTO,
                           "Symbol was resolved with mismatched type");
                    return type;
                },
                [this](HasNameOnly auto& sym) -> type& {
                    ASSERT(resolving_.has_sema_type(sym.name), "Name-only sym was never typed");
                    return resolving_.get_sema_type(sym.name);
                },
                [this](symbols::for_loop_capture& capture) -> type& {
                    ASSERT(capture.payload.is<ast::identifier_expr>(),
                           "Capture payload must be an ident");
                    ASSERT(resolving_.has_sema_type(capture.payload),
                           "For loop capture was never typed");
                    return resolving_.get_sema_type(capture.payload);
                }));
        break;
    case symbol_status::RESOLVING:
        if (const auto forwarded_type{forward_type(resolving_, stdx::none, sym)}) {
            resolving_.set_sema_type_if(id, *forwarded_type);
            return last_type_.emplace(*forwarded_type);
        }

        ctx_.poison_symbol(sym);
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("'{}' is used during its own resolution", sym.get_name()),
                             error::CYCLIC_DEPENDENCY,
                             resolving_.ast.location_of(id)));
    case symbol_status::UNRESOLVED: {
        sym.set_status(symbol_status::RESOLVING);

        // All other symbol data kinds are independently resolved
        const auto node{symbol_data.as_opt<symbols::node_t>()};
        ASSERT(node, "Unresolved symbol is not AST-associated");
        resolve(*node);
        resolving_.set_sema_type(id, *last_type_.take());
        break;
    }
    default: UNREACHABLE("Symbol status should only be 1 of 3 states");
    }

    if (sym.get_kind() == symbol_kind::POISONED) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id));
    }
    last_type_.emplace(resolving_.get_sema_type(id));
}

VISITOR_TEMPLATE_INIT(type_resolver, resolve_symbol, symbol&)

template <ast::IndexableID ID>
auto type_resolver::resolve_ident(ID id, const ast::identifier_expr& ident) -> void {
    const auto name{ident.name};
    auto       symbol_opt{ctx_.registry.lookup(table_stack_, name)};

    // Check for an undeclared identifier and poison the ident
    if (!symbol_opt) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("Use of undeclared identifier '{}'", name),
                             error::UNDECLARED_IDENTIFIER,
                             resolving_.ast.location_of(id)));
    }
    resolve_symbol(id, *symbol_opt);
}

VISITOR_TEMPLATE_INIT(type_resolver, resolve_ident, const ast::identifier_expr&)

auto type_resolver::visit(ast::node_id id, const ast::identifier_expr& ident) -> void {
    PROFILE_FUNCTION();
    resolve_ident(id, ident);
}

auto type_resolver::visit(ast::node_id id, const ast::if_expr& if_expr) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(if_expr.condition);
    TRY_RESOLVE(if_expr.consequence);

    // There can only be a non-void type with an alternate but this is for pass 3
    auto& branch_type{*last_type_.take()};
    if (if_expr.alternate) { TRY_RESOLVE(*if_expr.alternate); }
    resolving_.set_sema_type(id, branch_type);
    last_type_.emplace(branch_type);
}

auto type_resolver::visit(ast::node_id id, const ast::index_expr& index) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(index.array);
    auto& array_type{*last_type_.take()};
    auto& array_data{array_type.get_data()};

    if (const auto slice{array_data.as_opt<types::slice>()}) {
        last_type_.emplace(slice->underlying);
    } else if (const auto array{array_data.as_opt<types::array>()}) {
        last_type_.emplace(array->underlying);
    } else if (const auto deferred{array_data.as_opt<types::deferred_array>()}) {
        last_type_.emplace(deferred->underlying);
    } else if (const auto pointer{array_data.as_opt<types::pointer>()}) {
        last_type_.emplace(pointer->underlying);
    } else {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("Can only index slices, arrays, and pointers; found '{}'",
                                         type_kind_display_name(array_type.get_kind())),
                             error::TYPE_MISMATCH,
                             resolving_.ast.location_of(index.array)));
    }

    auto& single_item_type{*last_type_.take()};
    TRY_RESOLVE(index.index);
    auto& access_type{*last_type_.take()};

    // There may be a slice accessor which results in a slice type
    if (access_type.get_data().is<types::slice>()) {
        last_type_.emplace(ctx_.get_slice(types::mut::CONSTANT, false, single_item_type));
    } else {
        last_type_.emplace(single_item_type);
    }

    auto& result_type{*last_type_.take()};
    resolving_.set_sema_type(id, result_type);
    last_type_.emplace(result_type);
}

auto type_resolver::visit(ast::node_id id, const ast::infinite_loop_expr& loop) -> void {
    PROFILE_FUNCTION();
    auto&       loop_type{resolving_.get_sema_type(id)};
    const scope s{table_stack_, loop_type.get_symbol_table_idx(), table_idx_};

    // Just an abridged normal loop handler
    const auto& block{resolving_.ast.get_as<ast::block_stmt>(loop.block)};
    for (const auto& stmt : block) { TRY_RESOLVE(stmt); }
    last_type_.emplace(loop_type);
}

auto type_resolver::visit(ast::node_id id, const ast::assignment_expr& assign) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(assign.lhs);
    auto& lhs_type{*last_type_.take()};
    {
        const structural_guard g{implicit_type_stack_, lhs_type};
        TRY_RESOLVE(assign.rhs);
    }

    // Only pass 3 can verify assignment allowance due to mutability semantics
    resolving_.set_sema_type(id, *last_type_);
}

auto type_resolver::visit(ast::node_id id, const ast::binary_expr& binary) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(binary.lhs);
    TRY_RESOLVE(binary.rhs);
    resolving_.set_sema_type(id, *last_type_);
}

auto type_resolver::resolve_structural_access(type&                          object_type,
                                              ast::identifier_handle         member,
                                              source_location                object_location,
                                              stdx::option<std::string_view> object_name)
    -> stdx::result<gsl::not_null<type*>, diagnostic> {
    // Early validation to simplify error handling
    auto&      object_data{object_type.get_data()};
    const auto enum_type{object_data.as_opt<types::enum_t>()};
    const auto struct_type{object_data.as_opt<types::struct_t>()};
    const auto union_type{object_data.as_opt<types::union_t>()};
    if (!enum_type && !struct_type && !union_type) {
        return make_sema_err(
            fmt::format(
                "Can only access inner objects inside of structs, unions, and enums; found '{}'",
                type_kind_display_name(object_type.get_kind())),
            error::TYPE_MISMATCH,
            object_location);
    }

    ASSERT(object_type.has_symbol_table_idx(), "Structural should have a resolved table index");
    const auto table_idx{object_type.get_symbol_table_idx()};
    auto&      table{ctx_.registry.get(table_idx)};

    const auto& member_ident{resolving_.ast.get_as<ast::identifier_expr>(member)};
    const scope s{table_stack_, table_idx, table_idx_};
    auto        symbol_proxy{table.get_proxy_opt(member_ident.name)};

    if (!symbol_proxy) {
        return make_sema_err(
            object_name
                .transform([&](std::string_view name) -> std::string {
                    return fmt::format(
                        "Type '{}' has no field named '{}'", name, member_ident.name);
                })
                .value_or(fmt::format("Type has no field named '{}'", member_ident.name)),
            error::UNDECLARED_IDENTIFIER,
            resolving_.ast.location_of(member));
    }

    auto& [member_symbol, member_idx] = *symbol_proxy;
    gsl::not_null<type*> result_type  = &ctx_.get_poison();
    if (member_symbol.get_kind() == symbol_kind::POISONED) { return result_type; }

    if (enum_type) { return &enum_type->type_at(member_idx, object_type); }
    if (struct_type) { return &struct_type->type_at(member_idx); }
    if (union_type) { return &union_type->type_at(member_idx); }
    UNREACHABLE("Error handling failed to catch invalid type");
}

auto type_resolver::get_rightmost_name(ast::outer_access_handle handle) const noexcept
    -> std::string_view {
    auto current{handle};
    while (true) {
        if (const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(current)}) {
            return ident->name;
        }

        if (const auto scope{resolving_.ast.get_as_opt<ast::module_access_expr>(current)}) {
            current = scope->inner;
            continue;
        }

        if (const auto dot{resolving_.ast.get_as_opt<ast::dot_expr>(current)}) {
            current = dot->member;
            continue;
        }
        UNREACHABLE("OuterAccessHandle has violated an invariant of the Handle class");
    }
}

template <ast::IndexableID ID>
auto type_resolver::resolve_dot(ID id, const ast::dot_expr& dot) -> void {
    resolve(dot.object);
    if (last_type_->is_poison()) { return resolving_.set_sema_type(id, *last_type_); }
    auto& object_type{*last_type_.take()};

    auto result{resolve_structural_access(object_type,
                                          dot.member,
                                          resolving_.ast.location_of(dot.object),
                                          get_rightmost_name(dot.object))};
    if (!result) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(result).error()));
    }

    if (const auto struct_type{object_type.get_data().as_opt<types::struct_t>()}) {
        const auto& table{ctx_.registry.get(object_type.get_symbol_table_idx())};
        const auto& member_ident{resolving_.ast.get_as<ast::identifier_expr>(dot.member)};
        if (auto proxy = table.get_proxy_opt(member_ident.name)) {
            const auto& [member_symbol, member_idx] = *proxy;
            if (&struct_type->enclosing != &resolving_ &&
                member_idx < struct_type->ast_fields.size() &&
                !struct_type->ast_fields[member_idx].is_public()) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     fmt::format("Field '{}' of struct '{}' is private",
                                                 member_ident.name,
                                                 get_rightmost_name(dot.object)),
                                     error::ILLEGAL_PRIVATE_ACCESS,
                                     resolving_.ast.location_of(dot.member)));
            }
        }
    }

    // The structural resolver returns poisoned types in error conditions which can be bubbled here
    auto& member_type{*result.value()};
    resolving_.set_sema_type(dot.member, member_type);
    resolving_.set_sema_type(id, member_type);
    last_type_.emplace(member_type);
}

VISITOR_TEMPLATE_INIT(type_resolver, resolve_dot, const ast::dot_expr&)

auto type_resolver::visit(ast::node_id id, const ast::dot_expr& dot) -> void {
    PROFILE_FUNCTION();
    resolve_dot(id, dot);
}

auto type_resolver::visit(ast::node_id id, const ast::range_expr& range) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(range.lhs);
    auto& lhs_type{*last_type_.take()};
    TRY_RESOLVE(range.rhs);

    // Due to deferred type checking just use one type
    auto& slice_type{ctx_.get_slice(types::mut::CONSTANT, false, lhs_type)};
    resolving_.set_sema_type(id, slice_type);
    last_type_.emplace(slice_type);
}

namespace {

[[nodiscard]] constexpr auto plurality(const auto& vec) noexcept -> std::string_view {
    return vec.size() == 1 ? "" : "s";
}

} // namespace

auto type_resolver::validate_struct_initializer(ast::node_id                 init_id,
                                                const ast::initializer_expr& init,
                                                type&                        struct_type)
    -> stdx::result<void, diagnostic> {
    struct_validator_.clear();
    const auto& struct_data{struct_type.get_data().as<types::struct_t>()};

    // Check for duplicates
    for (const auto& [accessor, value] : init.initializers) {
        const auto  accessor_node{resolving_.ast.get_as<ast::implicit_access_expr>(accessor)};
        const auto& accessor_ident =
            resolving_.ast.get_as<ast::identifier_expr>(accessor_node.member);
        const auto field_name{accessor_ident.name};
        if (!struct_validator_.seen.insert(field_name).second) {
            struct_validator_.duplicates.emplace_back(field_name);
        }
        struct_validator_.provided.emplace_back(field_name);
    }

    if (!struct_validator_.duplicates.empty()) {
        return diagnostic{fmt::format("Struct initializer contains duplicate field{}: {}",
                                      plurality(struct_validator_.duplicates),
                                      fmt::join(struct_validator_.duplicates, ", ")),
                          error::DUPLICATE_FIELD,
                          resolving_.ast.location_of(init_id)};
    }

    // Check for missing fields
    const auto& enclosing{struct_data.enclosing};
    for (const auto& [ident, _, default_value] : struct_data.ast_fields) {
        const auto& field_node{enclosing.ast.get_as<ast::identifier_expr>(ident)};
        if (!default_value && !struct_validator_.seen.contains(field_node.name)) {
            struct_validator_.missings.emplace_back(field_node.name);
        }
    }

    if (!struct_validator_.missings.empty()) {
        return diagnostic{fmt::format("Struct initializer missing required field{}: {}",
                                      plurality(struct_validator_.missings),
                                      fmt::join(struct_validator_.missings, ", ")),
                          error::MISSING_FIELD,
                          resolving_.ast.location_of(init_id)};
    }

    // Check for extra/unknown fields
    const auto  struct_table_idx{struct_type.get_symbol_table_idx()};
    const auto& struct_table{ctx_.registry.get(struct_table_idx)};
    for (const auto name : struct_validator_.provided) {
        if (!struct_table.has(name)) { struct_validator_.unknowns.emplace_back(name); }
    }

    if (!struct_validator_.unknowns.empty()) {
        return diagnostic{fmt::format("Struct initializer contains unknown field{}: {}",
                                      plurality(struct_validator_.unknowns),
                                      fmt::join(struct_validator_.unknowns, ", ")),
                          error::UNKNOWN_FIELD,
                          resolving_.ast.location_of(init_id)};
    }
    return {};
}

auto type_resolver::visit(ast::node_id id, const ast::initializer_expr& init) -> void {
    PROFILE_FUNCTION();

    // Resolve the object first so it can be tied to the member's types
    stdx::option<type&> object_type_opt;
    if (init.object_type) {
        TRY_RESOLVE(*init.object_type);
        object_type_opt.emplace(*last_type_.take());
    } else if (const auto implicit_type{implicit_type_stack_.peek()}) {
        object_type_opt.emplace(*implicit_type);
    } else {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "Initializer expression requires a known type; provide an explicit "
                             "type or use in a typed context",
                             error::TYPE_MISMATCH,
                             resolving_.ast.location_of(id)));
    }

    type& object_type{*object_type_opt};
    if (!object_type.is_resolved()) {
        return last_type_.emplace(ctx_.poison_node(resolving_,
                                                   id,
                                                   "Cannot initialize an incomplete type",
                                                   error::CYCLIC_DEPENDENCY,
                                                   resolving_.ast.location_of(id)));
    }

    const auto  num_initializers{init.initializers.size()};
    const auto& object_data{object_type.get_data()};
    if (object_data.is<types::enum_t>()) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "Enums cannot be initialized with an initializer expression as "
                             "they lack member variables",
                             error::ARITY_MISMATCH,
                             resolving_.ast.location_of(id)));
    }
    if (object_data.is<types::union_t>()) {
        // This is a restriction naturally imposed by the definition of a union in theory
        if (num_initializers != 1) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("Union initializer lists must list exactly one field; found {}",
                            num_initializers),
                error::ARITY_MISMATCH,
                resolving_.ast.location_of(id)));
        }
    } else if (const auto struct_data{object_data.as_opt<types::struct_t>()}) {
        if (auto valid{validate_struct_initializer(id, init, object_type)}; !valid) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(valid).error()));
        }
    }

    if (!object_data.is<types::struct_t>() && !object_data.is<types::union_t>()) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("Only struct and union types may be used in "
                                         "initializer expressions; found '{}'",
                                         type_kind_display_name(object_type.get_kind())),
                             error::TYPE_MISMATCH,
                             resolving_.ast.location_of(id)));
    }

    for (const auto& [accessor, value] : init.initializers) {
        // The accessor is always a lookup into the stack
        {
            const structural_guard g{implicit_type_stack_, object_type};
            TRY_RESOLVE(accessor);
        }

        // The value might be a necessary implicit access
        auto& member_type{resolving_.get_sema_type(accessor)};
        if (member_type.is_poison()) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id));
        }

        {
            const structural_guard g{implicit_type_stack_, member_type};
            TRY_RESOLVE(value);
        }
    }

    resolving_.set_sema_type(id, object_type);
    last_type_.emplace(object_type);
}

auto type_resolver::visit(ast::node_id id, const ast::label_expr& label) -> void {
    PROFILE_FUNCTION();
    auto&       label_type{resolving_.get_sema_type(id)};
    const scope s{table_stack_, label_type.get_symbol_table_idx(), table_idx_};

    // Resolve the body but cache the label's type so the result can bind to the label
    TRY_RESOLVE(*label.body);
    auto sym{resolve_symbol_info(label.name, stdx::none)};
    if (!sym) { return; }
    auto& label_data{symbols::label::from(*sym)};

    // Labels may not be used outside of continues which don't push void
    if (!label_data.has_yield_types()) {
        label_data.add_yield_type(ctx_.get_builtin_resolved_type(type_kind::VOID));
    }

    // The last type inherits the result type to help propagation of poison
    auto& result_type{*label_data.get_yield_types()[0]};
    ASSERT(result_type.is_resolved(), "The label's inner type should've been resolved");
    label_type.resolve_if<type::data_t>(result_type.get_data());
    resolving_.set_sema_type(label.name, result_type);
    last_type_.emplace(result_type);
}

namespace {

// Collects potential duplicate implicit access match arms for the structural type
auto gather_arm_duplicates(gsl::span<const ast::match_expr::arm> arms,
                           mod::module&                          resolving,
                           type_resolver::structural_validator&  validator,
                           bool require_implicit_access) -> stdx::option<diagnostic> {
    for (const auto& arm : arms) {
        if (arm.pattern.is<ast::discarded>()) { continue; }

        // It's only possible to verify access expressions
        const auto pattern_node{resolving.ast.get_as_opt<ast::implicit_access_expr>(arm.pattern)};
        if (!pattern_node) {
            if (require_implicit_access) {
                return diagnostic{
                    "Match arm may only have an implicit access pattern in this context",
                    error::ILLEGAL_MATCH_PATTERN,
                    resolving.ast.location_of(arm.pattern)};
            }
            continue;
        }

        const auto& ident{resolving.ast.get_as<ast::identifier_expr>(pattern_node->member)};
        if (!validator.seen.insert(ident.name).second) {
            validator.duplicates.emplace_back(ident.name);
        }
        validator.provided.emplace_back(ident.name);
    }
    return stdx::none;
}

} // namespace

auto type_resolver::validate_enum_arms(ast::node_id           match_id,
                                       const ast::match_expr& match,
                                       type& enum_type) -> stdx::option<diagnostic> {
    enum_validator_.clear();

    if (enum_type.get_data().as<types::enum_t>().non_exhaustive && !match.catch_all_idx) {
        return diagnostic{"Match expressions over non-exhaustive enums must have a catch all arm",
                          error::ILLEGAL_MATCH_PATTERN,
                          resolving_.ast.location_of(match_id)};
    }

    // Track seen and duplicate variants in the match arms
    const auto diag{gather_arm_duplicates(match.arms, resolving_, enum_validator_, false)};
    ASSERT(!diag, "Enum validation should not return a diagnostic");
    if (!enum_validator_.duplicates.empty()) {
        return diagnostic{fmt::format("Match expression contains duplicate enumeration{}: {}",
                                      plurality(enum_validator_.duplicates),
                                      fmt::join(enum_validator_.duplicates, ", ")),
                          error::DUPLICATE_ENUMERATION,
                          resolving_.ast.location_of(match_id)};
    }

    // Check for unknown/extra variants
    const auto  enum_table_idx{enum_type.get_symbol_table_idx()};
    const auto& enum_table{ctx_.registry.get(enum_table_idx)};
    for (const auto name : enum_validator_.provided) {
        if (!enum_table.has(name)) { enum_validator_.unknowns.emplace_back(name); }
    }

    if (!enum_validator_.unknowns.empty()) {
        return diagnostic{fmt::format("Match expression contains unknown enumeration{}: {}",
                                      plurality(enum_validator_.unknowns),
                                      fmt::join(enum_validator_.unknowns, ", ")),
                          error::UNKNOWN_ENUMERATION,
                          resolving_.ast.location_of(match_id)};
    }
    return stdx::none;
}

auto type_resolver::validate_union_arms(ast::node_id           match_id,
                                        const ast::match_expr& match,
                                        type& union_type) -> stdx::option<diagnostic> {
    union_validator_.clear();
    const auto& union_data{union_type.get_data().as<types::union_t>()};

    // Track seen and duplicate fields in the match arms
    if (auto diag{gather_arm_duplicates(match.arms, resolving_, union_validator_, true)}; diag) {
        return diag;
    }

    if (!union_validator_.duplicates.empty()) {
        return diagnostic{fmt::format("Match expression contains duplicate union field{}: {}",
                                      plurality(union_validator_.duplicates),
                                      fmt::join(union_validator_.duplicates, ", ")),
                          error::DUPLICATE_FIELD,
                          resolving_.ast.location_of(match_id)};
    }

    // Check for missing fields only if there's a missing catch all
    if (!match.catch_all_idx) {
        const auto& enclosing{union_data.enclosing};
        for (const auto& [ident, _] : union_data.ast_fields) {
            const auto& field_node{enclosing.ast.get_as<ast::identifier_expr>(ident)};
            if (!union_validator_.seen.contains(field_node.name)) {
                union_validator_.missings.emplace_back(field_node.name);
            }
        }

        if (!union_validator_.missings.empty()) {
            return diagnostic{
                fmt::format("Match expression is non-exhaustive; missing union field{}: {}",
                            plurality(union_validator_.missings),
                            fmt::join(union_validator_.missings, ", ")),
                error::MISSING_FIELD,
                resolving_.ast.location_of(match_id)};
        }
    }

    // Check for unknown fields
    const auto  union_table_idx{union_type.get_symbol_table_idx()};
    const auto& union_table{ctx_.registry.get(union_table_idx)};
    for (const auto name : union_validator_.provided) {
        if (!union_table.has(name)) { union_validator_.unknowns.emplace_back(name); }
    }

    if (!union_validator_.unknowns.empty()) {
        return diagnostic{fmt::format("Match expression contains unknown union field{}: {}",
                                      plurality(union_validator_.unknowns),
                                      fmt::join(union_validator_.unknowns, ", ")),
                          error::UNKNOWN_FIELD,
                          resolving_.ast.location_of(match_id)};
    }
    return stdx::none;
}

auto type_resolver::visit(ast::node_id id, const ast::match_expr& match) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(match.matcher);
    auto&                  matcher_type{*last_type_.take()};
    const structural_guard g{implicit_type_stack_, matcher_type};

    // The expression must resolve to a single type on pass 3
    stdx::option<type&> first_type;
    const auto          try_set_first_type = [&] -> void {
        if (!first_type) {
            first_type = last_type_.take();
        } else {
            last_type_.reset();
        }
    };

    // Rip through the arms once to validate structural arm rules
    const auto& matcher_data{matcher_type.get_data()};
    if (matcher_data.is<types::enum_t>()) {
        if (auto diag{validate_enum_arms(id, match, matcher_type)}; diag) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(diag).value()));
        }
    } else if (matcher_data.is<types::union_t>()) {
        if (auto diag{validate_union_arms(id, match, matcher_type)}; diag) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(diag).value()));
        }
    } else if (matcher_data.is<types::builtin_type>()) {
        // It's assumed that any sufficiently large type cannot be fully enumerated
        stdx::option<u16> required_arm_count;
        switch (matcher_type.get_kind()) {
        case type_kind::I32:
        case type_kind::I64:
        case type_kind::ISIZE:
        case type_kind::U32:
        case type_kind::U64:
        case type_kind::USIZE: break;
        case type_kind::U8:    required_arm_count.emplace(256); break;
        case type_kind::BOOL:  required_arm_count.emplace(2); break;
        case type_kind::F32:
        case type_kind::F64:
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 "Cannot match on floats due to precision; use an if statement "
                                 "with explicit precision handling",
                                 sema::error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(match.matcher)));
        case type_kind::VOID:
        case type_kind::UNDEFINED:
            return last_type_.emplace(ctx_.poison_node(resolving_,
                                                       id,
                                                       "Empty types cannot be matched on",
                                                       sema::error::TYPE_MISMATCH,
                                                       resolving_.ast.location_of(match.matcher)));
        case type_kind::TYPE:
        case type_kind::AUTO:
        case type_kind::OPAQUE:
        case type_kind::NORETURN:
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("Can only match on integers, bytes, and booleans; found '{}'",
                            type_kind_display_name(matcher_type.get_kind())),
                sema::error::TYPE_MISMATCH,
                resolving_.ast.location_of(match.matcher)));
        default: UNREACHABLE("Builtin types should never take this type kind");
        }

        if (!match.catch_all_idx) {
            // With a required arm count the counts must match
            if (required_arm_count && required_arm_count != match.arms.size()) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    fmt::format("Matching on type '{}' requires a catch all arm with "
                                "a pattern of '_' or exactly {} patterned arms",
                                type_kind_display_name(matcher_type.get_kind()),
                                *required_arm_count),
                    sema::error::TYPE_MISMATCH,
                    resolving_.ast.location_of(match.matcher)));
            }

            if (!required_arm_count) {
                // Otherwise there must always be a catch all arm
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    fmt::format(
                        "Matching on type '{}' requires a catch all arm with a pattern of '_'",
                        type_kind_display_name(matcher_type.get_kind())),
                    sema::error::TYPE_MISMATCH,
                    resolving_.ast.location_of(match.matcher)));
            }
        }
    } else {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            fmt::format("Can only match on enums, unions, and certain primitive types; found '{}'",
                        type_kind_display_name(matcher_type.get_kind())),
            sema::error::TYPE_MISMATCH,
            resolving_.ast.location_of(match.matcher)));
    }

    // Each arm was assigned a new scope index on the first pass
    for (const auto& arm : match.arms) {
        // Tabled types have prefilled types that should be pushed on the table stack
        auto&       arm_type{resolving_.get_sema_type(arm)};
        const scope scope{table_stack_, arm_type.get_symbol_table_idx(), table_idx_};

        if (arm.capture && arm.capture->is<ast::identifier_expr>()) {
            // Unions implicitly unpack the value since the field is guaranteed to be valid
            if (const auto union_data{matcher_data.as_opt<types::union_t>()}) {
                // At this point the pattern is guaranteed to be an implicit access
                const auto implicit_access{
                    resolving_.ast.get_as_opt<ast::implicit_access_expr>(arm.pattern)};
                ASSERT(implicit_access, "Union validator failed to error");
                const auto& ident =
                    resolving_.ast.get_as<ast::identifier_expr>(implicit_access->member);

                const auto& table{ctx_.registry.get(matcher_type.get_symbol_table_idx())};
                const auto& proxy{table.get_proxy(ident.name)};
                resolving_.set_sema_type(*arm.capture, union_data->type_at(proxy.index));
            } else {
                resolving_.set_sema_type(*arm.capture, matcher_type);
            }

            resolve_symbol_info(*arm.capture, symbol_kind::VALUE);
        }

        if (!arm.pattern.is<ast::discarded>()) { TRY_RESOLVE(arm.pattern); }
        TRY_RESOLVE(arm.dispatch);

        // Set the arms type to the dispatch only if its not occupied by a tabled type
        try_set_first_type();
    }

    // In the rare case that a type could not be found we have to poison
    if (first_type) {
        resolving_.set_sema_type(id, *first_type);
        last_type_.emplace(*first_type);
    } else {
        last_type_.emplace(ctx_.poison_node(resolving_, id));
    }
}

namespace {

// Returns the mutability associated with the reference/address-of operator
[[nodiscard]] auto ref_addr_of_is_mutable(ast::node_id id) noexcept -> types::mutability_modifiers {
    using syntax::token_type_t;
    switch (id.get_token_type()) {
    case token_type_t::BW_AND:    return types::mut::CONSTANT;
    case token_type_t::AND_MUT:   return types::mut::MUTABLE;
    case token_type_t::CARET:     return types::mut::CONSTANT;
    case token_type_t::CARET_MUT: return types::mut::MUTABLE;
    default:                      UNREACHABLE("Invalid token types should be pruned prior to this function");
    }
}

} // namespace

auto type_resolver::visit(ast::node_id id, const ast::reference_expr& ref) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(ref.rhs);
    auto& rhs_type{*last_type_.take()};

    auto& new_type{ctx_.get_reference(ref_addr_of_is_mutable(id), rhs_type)};
    new_type.resolve<types::reference>(rhs_type);

    resolving_.set_sema_type(id, new_type);
    last_type_.emplace(new_type);
}

auto type_resolver::visit(ast::node_id id, const ast::address_of_expr& adr_of) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(adr_of.rhs);
    auto& rhs_type{*last_type_.take()};

    auto& new_type{ctx_.get_pointer(ref_addr_of_is_mutable(id), rhs_type)};
    new_type.resolve_if<types::pointer>(rhs_type);

    resolving_.set_sema_type(id, new_type);
    last_type_.emplace(new_type);
}

auto type_resolver::visit(ast::node_id id, const ast::dereference_expr& deref) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(deref.rhs);
    auto& rhs_type{*last_type_.take()};

    // Check for a pointer and update to the underlying type to enforce dereference semantics
    if (const auto pointer{rhs_type.get_data().as_opt<types::pointer>()}) {
        last_type_.emplace(pointer->underlying);
    } else {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("Cannot dereference non-pointer expression; found '{}'",
                                         type_kind_display_name(rhs_type.get_kind())),
                             error::TYPE_MISMATCH,
                             resolving_.ast.location_of(id)));
    }
    resolving_.set_sema_type(id, *last_type_);
}

auto type_resolver::visit(ast::node_id id, const ast::unary_expr& node) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(node.rhs);
    resolving_.set_sema_type(id, *last_type_);
}

auto type_resolver::visit(ast::node_id id, const ast::implicit_access_expr& implicit_access)
    -> void {
    PROFILE_FUNCTION();
    const auto implicit_type{implicit_type_stack_.peek()};
    if (!implicit_type) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "Implicit access expression used outside of a typed context",
                             error::TYPE_MISMATCH,
                             resolving_.ast.location_of(id)));
    }

    auto result{resolve_structural_access(
        *implicit_type, implicit_access.member, resolving_.ast.location_of(id))};
    if (!result) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(result).error()));
    }

    auto& member_type{*result.value()};
    resolving_.set_sema_type(implicit_access.member, member_type);
    resolving_.set_sema_type(id, member_type);
    last_type_.emplace(member_type);
}

// String literals are just constant arrays of bytes
auto type_resolver::visit(ast::node_id id, const ast::string_expr& string) -> void {
    PROFILE_FUNCTION();

    // String literals are null terminated since they can be trivially shortened to non null
    auto& type{ctx_.get_array(types::mut::CONSTANT,
                              true,
                              string.value.size() + 1,
                              ctx_.get_builtin_resolved_type(type_kind::U8))};

    // String literals with the same size will always have the same type
    resolving_.set_sema_type(id, type);
    last_type_.emplace(type);
}

#define MAKE_PRIMITIVE_RESOLVER(NodeType, kind)                                \
    auto type_resolver::visit(ast::node_id id, const ast::NodeType&) -> void { \
        PROFILE_FUNCTION();                                                    \
        last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::kind));   \
        last_type_->resolve_if<types::builtin_type>();                         \
        resolving_.set_sema_type(id, *last_type_);                             \
    }

MAKE_PRIMITIVE_RESOLVER(i32_expr, I32)
MAKE_PRIMITIVE_RESOLVER(i64_expr, I64)
MAKE_PRIMITIVE_RESOLVER(isize_expr, ISIZE)
MAKE_PRIMITIVE_RESOLVER(u32_expr, U32)
MAKE_PRIMITIVE_RESOLVER(u64_expr, U64)
MAKE_PRIMITIVE_RESOLVER(usize_expr, USIZE)
MAKE_PRIMITIVE_RESOLVER(u8_expr, U8)
MAKE_PRIMITIVE_RESOLVER(bool_expr, BOOL)
MAKE_PRIMITIVE_RESOLVER(void_expr, VOID)
MAKE_PRIMITIVE_RESOLVER(undefined_expr, UNDEFINED)
MAKE_PRIMITIVE_RESOLVER(f32_expr, F32)
MAKE_PRIMITIVE_RESOLVER(f64_expr, F64)

template <ast::IndexableID ID>
auto type_resolver::resolve_module_access(ID id, const ast::module_access_expr& access) -> void {
    // Resolving the right hand side recurses down to the identifier level
    resolve(access.outer);
    if (last_type_->is_poison()) { return resolving_.set_sema_type(id, *last_type_); }
    auto& outer_type{*last_type_.take()};
    auto& outer_resolved{outer_type.get_data()};

    if (const auto module{outer_resolved.as_opt<types::module>()}) {
        // The module may not have been resolved yet due to order independence
        auto& inner_mod{module->imported};
        if (inner_mod.is_resolvable()) {
            context new_ctx{ctx_};
            resolve_types(inner_mod, new_ctx);
            if (inner_mod.is_poisoned()) {
                return last_type_.emplace(ctx_.poison_node(resolving_, id));
            }
        }

        // Step into the module's scope for lookup
        const auto& inner_ident{resolving_.ast.get_as<ast::identifier_expr>(access.inner)};
        auto        sym{ctx_.registry.get_from_opt(*inner_mod.root_table_idx, inner_ident.name)};
        if (!sym) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 fmt::format("Module '{}' has no member named '{}'",
                                             get_rightmost_name(access.outer),
                                             inner_ident.name),
                                 error::UNDECLARED_IDENTIFIER,
                                 resolving_.ast.location_of(access.inner)));
        }

        const auto symbol_node{sym->get_data().as_opt<symbols::node_t>()};
        if (!symbol_node) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }

        if (&inner_mod != &resolving_ && !sym->is_public(inner_mod)) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 fmt::format("Symbol '{}' is private to module '{}'",
                                             inner_ident.name,
                                             get_rightmost_name(access.outer)),
                                 error::ILLEGAL_PRIVATE_ACCESS,
                                 resolving_.ast.location_of(access.inner)));
        }

        stdx::option<ast::type_modifier> mod;
        if constexpr (ast::IndexableExplicitTypeID<ID>) { mod = id.get_modifier(); }
        switch (sym->get_status()) {
        case symbol_status::RESOLVING: {
            const auto poison_out = [&] -> void {
                ctx_.poison_symbol(*sym);
                last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    fmt::format(
                        "Cross-module cyclic dependency detected while resolving symbol '{}'",
                        inner_ident.name),
                    error::CYCLIC_DEPENDENCY,
                    resolving_.ast.location_of(access.inner)));
            };

            // Explicitly reject infinite size cycles across modules before forwarding
            if (!mod || (!mod->is_ptr() && !mod->is_ref())) { return poison_out(); }
            if (const auto forwarded_type{forward_type(inner_mod, mod, *sym)}) {
                resolving_.set_sema_type(access.inner, *forwarded_type);
                resolving_.set_sema_type(id, *forwarded_type);
                return last_type_.emplace(*forwarded_type);
            }

            return poison_out();
        }
        case symbol_status::UNRESOLVED: {
            type_resolver inner_resolver{inner_mod, ctx_};
            inner_resolver.resolve(*symbol_node);
            break;
        }
        case symbol_status::RESOLVED: break;
        }

        if (sym->get_kind() == symbol_kind::POISONED || !inner_mod.has_sema_type(*symbol_node)) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id));
        }

        auto& ident_type{inner_mod.get_sema_type(*symbol_node)};
        resolving_.set_sema_type(access.inner, ident_type);
        resolving_.set_sema_type(id, ident_type);
        return last_type_.emplace(ident_type);
    }

    if (outer_resolved.is<types::struct_t>() || outer_resolved.is<types::enum_t>() ||
        outer_resolved.is<types::union_t>()) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            fmt::format("Use the dot operator '.' to access {} fields; found module access '::'",
                        type_kind_display_name(outer_type.get_kind())),
            error::TYPE_MISMATCH,
            resolving_.ast.location_of(access.outer)));
    }

    return last_type_.emplace(ctx_.poison_node(
        resolving_,
        id,
        fmt::format("Module access operator '::' can only be applied to modules; found '{}'",
                    type_kind_display_name(outer_type.get_kind())),
        error::TYPE_MISMATCH,
        resolving_.ast.location_of(access.outer)));
}

VISITOR_TEMPLATE_INIT(type_resolver, resolve_module_access, const ast::module_access_expr&)

auto type_resolver::visit(ast::node_id id, const ast::module_access_expr& scope) -> void {
    PROFILE_FUNCTION();
    resolve_module_access(id, scope);
}

namespace {

[[nodiscard]] auto incomplete_field(std::string_view name, const source_location& location)
    -> diagnostic {
    return diagnostic{
        fmt::format("Field '{}' has an incomplete type; creates an infinite size cycle", name),
        error::CYCLIC_DEPENDENCY,
        location};
}

[[nodiscard]] auto illegal_auto_field(std::string_view       kind,
                                      std::string_view       name,
                                      const source_location& location) -> diagnostic {
    return diagnostic{fmt::format("{} field '{}' cannot have type 'auto'", kind, name),
                      error::ILLEGAL_AUTO_USAGE,
                      location};
}

} // namespace

template <ast::IndexableID ID>
auto type_resolver::visit(ID id, const ast::struct_expr& struct_expr) -> void {
    PROFILE_FUNCTION();
    auto&                  struct_type{resolving_.get_sema_type(id)};
    const scope            s{table_stack_, struct_type.get_symbol_table_idx(), table_idx_};
    const structural_guard g{user_type_stack_, struct_type};

    auto field_types{ctx_.pool.get_many_unsafe(struct_expr.fields.size())};
    for (usize i{0}; const auto& field : struct_expr.fields) {
        const auto& ident{resolving_.ast.get_as<ast::identifier_expr>(field.name)};
        auto        sym{ctx_.registry.get_from_opt(table_idx_, ident.name)};
        if (!sym) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }

        TRY_RESOLVE(field.explicit_type);
        auto* field_type{last_type_.take()};

        if (field_type->get_kind() == type_kind::AUTO) {
            if (!field.default_value) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    illegal_auto_field(
                        "Struct", ident.name, resolving_.ast.location_of(field.explicit_type))));
            }
            TRY_RESOLVE(*field.default_value);
            field_type = last_type_.take();
            if (field_type->get_kind() == type_kind::AUTO) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    illegal_auto_field(
                        "Struct", ident.name, resolving_.ast.location_of(field.explicit_type))));
            }
        } else if (field.default_value) {
            const structural_guard inner_g{implicit_type_stack_, *field_type};
            TRY_RESOLVE(*field.default_value);
        }

        if (!field_type->is_resolved()) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                incomplete_field(ident.name, resolving_.ast.location_of(field.explicit_type))));
        }

        resolving_.set_sema_type(field.name, *field_type);
        sym->set_kind(symbol_kind::VALUE);
        sym->set_status(symbol_status::RESOLVED);
        field_types[i++] = field_type;
    }

    auto member_types{ctx_.pool.get_many_unsafe(struct_expr.members.size())};
    committable_resolution<types::struct_t> resolution{
        struct_type, field_types, struct_expr.fields, member_types, resolving_};
    if (!resolve_members(member_types, struct_expr.members)) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id));
    }

    resolution.commit();
    last_type_.emplace(struct_type);
}

VISITOR_TEMPLATE_INIT(type_resolver, visit, const ast::struct_expr&)

template <ast::IndexableID ID>
auto type_resolver::visit(ID id, const ast::union_expr& union_expr) -> void {
    PROFILE_FUNCTION();
    auto&                  union_type{resolving_.get_sema_type(id)};
    const scope            s{table_stack_, union_type.get_symbol_table_idx(), table_idx_};
    const structural_guard g{user_type_stack_, union_type};

    auto field_types{ctx_.pool.get_many_unsafe(union_expr.fields.size())};
    for (usize i{0}; const auto& field : union_expr.fields) {
        const auto& ident{resolving_.ast.get_as<ast::identifier_expr>(field.name)};
        auto        sym{ctx_.registry.get_from_opt(table_idx_, ident.name)};
        if (!sym) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }

        TRY_RESOLVE(field.explicit_type);
        auto& field_type{*last_type_.take()};

        if (field_type.get_kind() == type_kind::AUTO) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                illegal_auto_field(
                    "Union", ident.name, resolving_.ast.location_of(field.explicit_type))));
        }

        if (!field_type.is_resolved()) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                incomplete_field(ident.name, resolving_.ast.location_of(field.explicit_type))));
        }

        resolving_.set_sema_type(field.name, field_type);
        sym->set_kind(symbol_kind::VALUE);
        sym->set_status(symbol_status::RESOLVED);
        field_types[i++] = &field_type;
    }

    auto member_types{ctx_.pool.get_many_unsafe(union_expr.members.size())};
    committable_resolution<types::union_t> resolution{
        union_type, field_types, union_expr.fields, member_types, resolving_};
    if (!resolve_members(member_types, union_expr.members)) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id));
    }

    resolution.commit();
    last_type_.emplace(union_type);
}

VISITOR_TEMPLATE_INIT(type_resolver, visit, const ast::union_expr&)

auto type_resolver::visit(ast::node_id id, const ast::while_loop_expr& while_loop) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(while_loop.condition);
    if (while_loop.continuation) { TRY_RESOLVE(*while_loop.continuation); }
    // The loop itself holds the block index which houses captures, not the block
    auto& loop_type{resolving_.get_sema_type(id)};
    {
        const scope s{table_stack_, loop_type.get_symbol_table_idx(), table_idx_};
        const auto& block{resolving_.ast.get_as<ast::block_stmt>(while_loop.block)};
        for (const auto& stmt : block) { TRY_RESOLVE(stmt); }
    }

    if (while_loop.non_break) { TRY_RESOLVE(*while_loop.non_break); }
    last_type_.emplace(loop_type);
}

// DONT CALL ME FROM ANY LOOP/CONDITION/FN RESOLVER
auto type_resolver::visit(ast::node_id id, const ast::block_stmt& block) -> void {
    PROFILE_FUNCTION();
    auto&       block_type{resolving_.get_sema_type(id)};
    const scope s{table_stack_, block_type.get_symbol_table_idx(), table_idx_};

    // Just an abridged loop handler
    for (const auto& stmt : block) { TRY_RESOLVE(stmt); }
    last_type_.emplace(block_type);
}

auto type_resolver::resolve_control_flow_label(stdx::option<ast::identifier_handle> label,
                                               std::string_view                     stmt_name)
    -> stdx::result<stdx::option<symbol&>, diagnostic> {
    if (label) {
        const auto& ident{resolving_.ast.get_as<ast::identifier_expr>(*label)};
        auto        sym{ctx_.registry.lookup(table_stack_, ident.name)};
        if (!sym || sym->get_kind() != symbol_kind::LABEL) {
            return make_sema_err(
                fmt::format("Labeled {} statements must be used with a known label", stmt_name),
                error::ILLEGAL_CONTROL_FLOW,
                resolving_.ast.location_of(*label));
        }
        return sym;
    }
    return stdx::none;
}

auto type_resolver::visit(ast::node_id id, const ast::break_stmt& break_stmt) -> void {
    PROFILE_FUNCTION();
    auto result{resolve_control_flow_label(break_stmt.label, "break")};
    if (!result) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(result).error()));
    }

    // No payload is semantically equivalent to breaking with void
    if (auto sym{result.value()}) {
        auto& label_data{symbols::label::from(*sym)};

        if (break_stmt.expression) {
            TRY_RESOLVE(*break_stmt.expression);
            auto& expression_type{*last_type_.take()};
            label_data.add_yield_type(expression_type);
            resolving_.set_sema_type(id, expression_type);
        } else {
            auto& void_type{ctx_.get_builtin_resolved_type(type_kind::VOID)};
            label_data.add_yield_type(void_type);
            resolving_.set_sema_type(id, void_type);
        }
    } else {
        resolving_.set_sema_type(id, ctx_.get_builtin_resolved_type(type_kind::VOID));
    }

    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::NORETURN));
}

auto type_resolver::visit(ast::node_id id, const ast::continue_stmt& continue_stmt) -> void {
    PROFILE_FUNCTION();
    auto result{resolve_control_flow_label(continue_stmt.label, "continue")};
    if (!result) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(result).error()));
    }

    resolving_.set_sema_type(id, ctx_.get_builtin_resolved_type(type_kind::VOID));
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::NORETURN));
}

auto type_resolver::visit(ast::node_id id, const ast::decl_stmt& decl) -> void {
    PROFILE_FUNCTION();
    const auto& ident{resolving_.ast.get_as<ast::identifier_expr>(*decl.name)};
    auto        symbol_opt{ctx_.registry.lookup(table_stack_, ident.name)};
    ASSERT(symbol_opt, "Somehow the declaration was lost in the symbol table");
    auto& sym{*symbol_opt};

    // Breaking out early is possible due to out of order semantics
    if (sym.get_status() == symbol_status::RESOLVED) {
        ASSERT(resolving_.has_sema_type(id), "Resolved decl has no type");
        return last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID));
    }
    sym.set_status(symbol_status::RESOLVING);

    const auto poison_out = [&] -> void {
        resolving_.set_sema_type(decl.name, *last_type_);
        ctx_.poison_symbol(sym);
        resolving_.set_sema_type(id, *last_type_);
    };

    {
        stdx::option<structural_guard> type_guard;

        // With an explicit type, the ident should always adopt that exact type
        if (decl.explicit_type) {
            resolve(*decl.explicit_type);
            if (last_type_->is_poison()) { return poison_out(); }
            auto& explicit_type{*last_type_.take()};
            if (explicit_type.get_kind() == type_kind::AUTO) {
                if (!decl.value) {
                    ctx_.poison_symbol(sym,
                                       "Type 'auto' requires an initializer expression",
                                       error::AUTO_WITHOUT_INITIALIZER,
                                       resolving_.ast.location_of(id));
                    resolving_.set_sema_type(decl.name, ctx_.get_poison());
                    return last_type_.emplace(ctx_.poison_node(resolving_, id));
                }
            } else {
                type_guard.emplace(implicit_type_stack_, explicit_type);
                resolving_.set_sema_type(id, explicit_type);
            }
        }

        // Only update the decl value type if it hasn't been set
        if (decl.value) {
            resolve(*decl.value);
            if (last_type_->is_poison()) { return poison_out(); }
            resolving_.set_sema_type_if(id, *last_type_.take());
        }
    }

    // The symbol's kind can be fully resolved with knowledge of the declarations type
    auto&       resolved_type{resolving_.get_sema_type(id)};
    const auto& type_data{resolved_type.get_data()};
    if (resolved_type.is_poison()) {
        sym.set_kind(symbol_kind::POISONED);
    } else {
        if (!sym.has_kind()) {
            if (type_data.is<types::builtin_function>() || type_data.is<types::function>()) {
                sym.set_kind(symbol_kind::CALLABLE);
            } else if (type_data.is<types::enum_t>() || type_data.is<types::struct_t>() ||
                       type_data.is<types::union_t>() ||
                       resolved_type == ctx_.get_builtin_resolved_type(type_kind::TYPE)) {
                sym.set_kind(symbol_kind::TYPE);
            } else if (type_data.is<types::module>()) {
                sym.set_kind(symbol_kind::MODULE);
            } else {
                sym.set_kind(symbol_kind::VALUE);
            }
        }

        if (type_data.is<types::function>()) {
            const auto& decl_ident{resolving_.ast.get_as<ast::identifier_expr>(decl.name)};
            ctx_.generic_functions.set_function_name(resolved_type, decl_ident.name);
        }
    }

    resolving_.set_sema_type_if(decl.name, resolved_type);
    sym.set_status(symbol_status::RESOLVED);
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID));
}

auto type_resolver::visit(ast::node_id id, const ast::defer_stmt& defer) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(defer.deferred);
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID));
}

auto type_resolver::visit(ast::node_id id, const ast::discard_stmt& discard) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(discard.discarded);
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID));
}

auto type_resolver::visit(ast::node_id id, const ast::expr_stmt& expr) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(expr.expression);
    resolving_.set_sema_type(expr.expression, *last_type_.take());
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID));
}

auto type_resolver::visit(ast::node_id id, const ast::import_stmt& import_stmt) -> void {
    PROFILE_FUNCTION();
    const auto [ident_id, name]{import_stmt.get_name(resolving_.ast)};
    auto& sym{ctx_.registry.get_from(table_idx_, name)};

    const auto poison_out = [&] -> void {
        last_type_.emplace(ctx_.get_poison());
        resolving_.set_sema_type(ident_id, *last_type_);
        ctx_.poison_symbol(sym);
    };

    // Updating this type reflects on the symbol in the actual table as well
    auto& import_type{resolving_.get_sema_type(id)};
    if (import_type.is_poison()) { return poison_out(); }
    ASSERT(import_type.is_resolved(), "Import types should be resolved on pass 1");
    auto& module{import_type.get_data().as<types::module>()};

    // There's no need to poison the import type since it would lose all of the module information
    context new_ctx{ctx_};
    resolve_types(module.imported, new_ctx);
    if (module.imported.is_poisoned()) { return poison_out(); }
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID));
}

auto type_resolver::visit(ast::node_id id, const ast::return_stmt& return_stmt) -> void {
    PROFILE_FUNCTION();
    if (return_stmt.expression) {
        TRY_RESOLVE(*return_stmt.expression);
        auto& return_expr_type{*last_type_.take()};
        resolving_.set_sema_type(id, return_expr_type);
        if (!return_trackers_.empty()) { return_trackers_.back().add_return(return_expr_type); }
    } else {
        auto& void_type{ctx_.get_builtin_resolved_type(type_kind::VOID)};
        resolving_.set_sema_type(id, void_type);
        if (!return_trackers_.empty()) { return_trackers_.back().add_return(void_type); }
    }
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::NORETURN));
}

auto type_resolver::visit(ast::node_id id, const ast::test_stmt& test) -> void {
    PROFILE_FUNCTION();
    auto&       test_type{resolving_.get_sema_type(id)};
    const scope s{table_stack_, test_type.get_symbol_table_idx(), table_idx_};

    // Duplicate test blocks are a big no-no if named
    if (test.description) {
        const auto& description{resolving_.ast.get_as<ast::string_expr>(*test.description)};
        const auto& name{description.value};
        auto [it, inserted]{named_tests_.try_emplace(name, id)};
        if (!inserted) {
            const auto original_loc{resolving_.ast.location_of(it->second)};
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("Duplicate test block named '{}'; previous declaration here: {}",
                            name,
                            original_loc),
                error::DUPLICATE_TEST_NAME,
                resolving_.ast.location_of(id)));
        }
    }

    const auto& block{resolving_.ast.get_as<ast::block_stmt>(test.block)};
    for (const auto& stmt : block) { TRY_RESOLVE(stmt); }
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID));
}

auto type_resolver::visit(ast::node_id id, const ast::using_stmt& using_stmt) -> void {
    PROFILE_FUNCTION();
    const auto& ident{resolving_.ast.get_as<ast::identifier_expr>(using_stmt.alias)};
    auto        sym{ctx_.registry.get_from_opt(table_idx_, ident.name)};
    if (!sym) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }
    if (sym->get_status() == symbol_status::RESOLVED) {
        ASSERT(resolving_.has_sema_type(id), "Resolved alias has no type");
        return last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID));
    }

    const auto poison_out = [&] -> void {
        resolving_.set_sema_type(using_stmt.alias, *last_type_);
        resolving_.set_sema_type(id, *last_type_);
        ctx_.poison_symbol(*sym);
    };

    // Bind the resolved type to the symbol now that its been collected
    sym->set_status(symbol_status::RESOLVING);
    resolve(using_stmt.explicit_type);
    if (last_type_->is_poison()) { return poison_out(); }
    auto& explicit_type{*last_type_.take()};
    if (explicit_type.get_kind() == type_kind::AUTO) {
        last_type_.emplace(ctx_.poison_node(resolving_,
                                            id,
                                            "Type aliases cannot be 'auto'",
                                            error::ILLEGAL_AUTO_USAGE,
                                            resolving_.ast.location_of(using_stmt.explicit_type)));
        return poison_out();
    }
    resolving_.set_sema_type(id, explicit_type);

    sym->set_status(symbol_status::RESOLVED);
    resolve(using_stmt.alias);
    if (last_type_->is_poison()) { return poison_out(); }
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID));
}

// Without a modifier or with poison the result should be the same as the node
auto type_resolver::apply_explicit_modifiers(ast::explicit_type_id id, type& inner_type) -> type& {
    const auto modifier{id.get_modifier()};
    if (modifier.is_value() || inner_type.is_poison()) { return inner_type; }
    const auto mutability{mutability_from_type_modifier(modifier)};

    // Conditionally update the mutability since the modifier might entail mutability or volatility
    auto new_key{inner_type.get_key()};
    if (mutability) { new_key.set_mut(*mutability); }

    // The key should reflect the new kind and should not double imprint the same type
    if (modifier.is_ptr()) {
        new_key.clear_markers();
        new_key.set_kind(type_kind::POINTER);
        new_key.imprint(inner_type);

        auto& new_ptr_type{*ctx_.pool[new_key]};
        new_ptr_type.resolve_if<types::pointer>(inner_type);
        return new_ptr_type;
    }

    if (modifier.is_ref()) {
        new_key.clear_markers();
        new_key.set_kind(type_kind::REFERENCE);
        new_key.imprint(inner_type);

        auto& new_ref_type{*ctx_.pool[new_key]};
        new_ref_type.resolve_if<types::reference>(inner_type);
        return new_ref_type;
    }

    if (modifier.is_volatile()) {
        // Volatility is baked into mutability and should not be imprinted
        auto& new_vol_type{*ctx_.pool[new_key]};
        new_vol_type.resolve_if<type::data_t>(inner_type.get_data());
        return new_vol_type;
    }
    UNREACHABLE("A new type modifier was likely added yet unaccounted for");
}

auto type_resolver::visit(ast::explicit_type_id id, const ast::identifier_expr& ident) -> void {
    PROFILE_FUNCTION();
    auto symbol_opt{ctx_.registry.lookup(table_stack_, ident.name)};
    if (!symbol_opt) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("Use of undeclared identifier '{}'", ident.name),
                             error::UNDECLARED_IDENTIFIER,
                             resolving_.ast.location_of(id)));
    }
    auto& sym{*symbol_opt};

    const auto forwarded_type{forward_type(resolving_, id.get_modifier(), sym)};
    forwarded_type ? last_type_.emplace(*forwarded_type) : resolve_ident(id, ident);

    auto& resolved{apply_explicit_modifiers(id, *last_type_.take())};
    resolving_.set_sema_type(id, resolved);
    last_type_.emplace(resolved);
}

#define MAKE_MODIFIED_RESOLVER(NodeType, resolver)                                           \
    auto type_resolver::visit(ast::explicit_type_id id, const ast::NodeType& node) -> void { \
        PROFILE_FUNCTION();                                                                  \
        resolver(id, node);                                                                  \
        auto& resolved{apply_explicit_modifiers(id, *last_type_.take())};                    \
        resolving_.set_sema_type(id, resolved);                                              \
        last_type_.emplace(resolved);                                                        \
    }

MAKE_MODIFIED_RESOLVER(module_access_expr, resolve_module_access)
MAKE_MODIFIED_RESOLVER(dot_expr, resolve_dot)
MAKE_MODIFIED_RESOLVER(call_expr, resolve_call)

auto type_resolver::visit(ast::explicit_type_id id, const ast::explicit_function_type& fn) -> void {
    PROFILE_FUNCTION();
    auto param_types{ctx_.pool.get_many_unsafe(fn.parameter_types.size())};

    // Make a unique function type by imprinting every associated type
    types::key_t fn_key{type_kind::FUNCTION, types::mut::CONSTANT};
    for (usize i{0}; const auto& param : fn.parameter_types) {
        TRY_RESOLVE(param);
        auto& param_type{*last_type_.take()};
        if (param_type.get_kind() == type_kind::AUTO) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 "Function types cannot have 'auto' parameter types",
                                 error::ILLEGAL_AUTO_USAGE,
                                 resolving_.ast.location_of(param)));
        }
        fn_key.imprint(param_type);
        param_types[i++] = &param_type;
    }

    TRY_RESOLVE(fn.explicit_return_type);
    auto& return_type{*last_type_.take()};
    if (return_type.get_kind() == type_kind::AUTO) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "Function types cannot have 'auto' return type",
                             error::ILLEGAL_AUTO_USAGE,
                             resolving_.ast.location_of(fn.explicit_return_type)));
    }
    fn_key.imprint(return_type);

    auto& resolved_fn{*ctx_.pool[fn_key]};
    resolved_fn.resolve_if<types::function>(false, param_types, return_type);

    auto& final_type{apply_explicit_modifiers(id, resolved_fn)};
    resolving_.set_sema_type(id, final_type);
    last_type_.emplace(final_type);
}

auto type_resolver::visit(ast::explicit_type_id id, ast::explicit_type_id nested) -> void {
    PROFILE_FUNCTION();
    resolve(nested);
    auto& resolved{apply_explicit_modifiers(id, *last_type_.take())};
    resolving_.set_sema_type(id, resolved);
    last_type_.emplace(resolved);
}

auto type_resolver::visit(ast::explicit_type_id id, const ast::explicit_array_type& array) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(array.inner_explicit_type);
    auto& item_type{*last_type_.take()};

    if (item_type.get_kind() == type_kind::AUTO) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "Array elements cannot have type 'auto'",
                             error::ILLEGAL_AUTO_USAGE,
                             resolving_.ast.location_of(array.inner_explicit_type)));
    }

    const auto null_terminated{array.null_terminated};
    if (array.dimension) {
        // This is not an error for slices since slices are just pointers with length
        if (!item_type.is_resolved()) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                incomplete_array_item(resolving_.ast.location_of(array.inner_explicit_type))));
        }

        TRY_RESOLVE(*array.dimension);
        last_type_.emplace(ctx_.pool[{type_kind::TYPE, types::mut::CONSTANT, &array}]);
        last_type_->resolve_if<types::deferred_array>(array, item_type);
    } else {
        last_type_.emplace(ctx_.get_slice(types::mut::CONSTANT, null_terminated, item_type));
    }

    auto& final_type{apply_explicit_modifiers(id, *last_type_.take())};
    resolving_.set_sema_type(id, final_type);
    last_type_.emplace(final_type);
}

auto type_resolver::instantiate_generic(type&                        callee_type,
                                        const generic_function_info& fn_info,
                                        gsl::span<type*>             concrete_args)
    -> stdx::option<gsl::not_null<type*>> {
    mod::module& fn_mod{*fn_info.module};
    const auto&  fn_expr{*fn_info.fn_expr};
    const auto   fn_type{fn_info.fn_type};
    const auto   fn_table_idx{fn_type->get_symbol_table_idx()};

    symbol_table_stack inst_stack;
    inst_stack.push(*ctx_.prelude_index);
    if (fn_mod.root_table_idx) { inst_stack.push(*fn_mod.root_table_idx); }
    inst_stack.push(fn_table_idx);

    ASSERT(fn_expr.parameters.size() == concrete_args.size(),
           "Arity should be validated in resolve_call");
    for (const auto& [arg_type, param] : std::views::zip(concrete_args, fn_expr.parameters)) {
        fn_mod.set_sema_type(param.name, *arg_type);
        const auto& ident{fn_mod.ast.get_as<ast::identifier_expr>(param.name)};
        if (auto sym{ctx_.registry.get_from_opt(fn_table_idx, ident.name)}) {
            sym->set_kind(symbol_kind::VALUE);
            sym->set_status(symbol_status::RESOLVED);
        }
    }

    type_resolver inst_resolver{fn_mod, ctx_, fn_table_idx, std::move(inst_stack)};
    inst_resolver.resolve(fn_expr.explicit_return_type);
    if (inst_resolver.last_type_->is_poison()) { return stdx::none; }
    auto&      return_type{*inst_resolver.last_type_.take()};
    const auto is_auto_return{return_type.get_kind() == type_kind::AUTO};
    inst_resolver.return_trackers_.emplace_back(return_tracker{
        .return_types   = {},
        .is_auto_return = is_auto_return,
    });

    const auto  diags_before{ctx_.diags.size()};
    const auto& block{fn_mod.ast.get_as<ast::block_stmt>(fn_expr.body)};
    bool        resolved_poison{false};
    for (const auto& stmt : block) {
        inst_resolver.resolve(stmt);
        if (inst_resolver.last_type_->is_poison()) { resolved_poison = true; }
    }
    if (ctx_.diags.size() > diags_before || resolved_poison) { return stdx::none; }

    auto tracker{std::move(inst_resolver.return_trackers_.back())};
    inst_resolver.return_trackers_.pop_back();
    auto& deduced_return_type{is_auto_return ? tracker.deduced_return_type(ctx_) : return_type};

    fn_mod.generic_instantiations.emplace_back(generic_instantiation_request{
        .generic_fn_type = &callee_type,
        .arg_types       = concrete_args,
        .return_type     = &deduced_return_type,
        .mangled_name =
            fmt::format("{}__{}",
                        fn_info.name.value_or("fn"),
                        fmt::join(concrete_args | std::views::transform([](const auto& arg) {
                                      return type_kind_display_name(arg->get_kind());
                                  }),
                                  "_")),
        .fn_node_id = fn_info.node_id,
    });
    return &deduced_return_type;
}

} // namespace ghoti::sema
