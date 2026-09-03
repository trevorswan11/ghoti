#include "compiler/sema/passes/type_resolver.hh"

#include <algorithm>
#include <cctype>
#include <concepts>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/assert.hh>
#include <stdx/enum.hh>
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
#include "compiler/gir/const_eval.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/generic.hh"
#include "compiler/sema/impl_registry.hh"
#include "compiler/sema/instantiation_cache.hh"
#include "compiler/sema/side_tables.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/token_type.hh"
#include "support/diagnostic.hh"

namespace ghoti::sema {

auto type_resolver::resolve_types(mod::module& module, context& ctx) -> mod::module_state {
    PROFILE_FUNCTION();

    // A module already poisoned by symbol collection stays poisoned
    const auto poisoned_collection{module.state == mod::module_state::POISONED_SYMBOL_COLLECTION};

    if (module.is_resolvable()) {
        module.state = poisoned_collection ? mod::module_state::POISONED_TYPE_RESOLVING
                                           : mod::module_state::TYPE_RESOLVING;
        ctx.inject_prelude();

        type_resolver resolver{module, ctx};
        resolver.pre_register_impls();
        for (const auto& node : module.ast) { resolver.resolve(node); }

        if (!ctx.diags.empty()) {
            return module.error_out(std::move(ctx.diags),
                                    mod::module_state::POISONED_TYPE_RESOLVED);
        }
        if (module.is_poisoned()) {
            module.state = mod::module_state::POISONED_TYPE_RESOLVED;
            return module.state;
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

[[nodiscard]] constexpr auto array_element_mutability(bool mut_elements) noexcept
    -> types::mutability_modifiers {
    return mut_elements ? types::mut::MUTABLE : types::mut::CONSTANT;
}

[[nodiscard]] auto container_element_mutability(const type& t) -> types::mutability_modifiers {
    if (const auto def{t.get_data().as_opt<types::deferred_array>()}) {
        return array_element_mutability(def->array.mut_elements);
    }
    return t.is_constant() ? types::mut::CONSTANT : types::mut::MUTABLE;
}

[[nodiscard]] auto denoted_type(type& t) -> type& {
    if (t.get_kind() == type_kind::TYPE) {
        if (const auto meta{t.get_data().as_opt<types::meta_type>()}) { return meta->instance; }
    }
    return t;
}

// The module a user-defined type was declared in, or null for a builtin / structural type.
[[nodiscard]] auto declaring_module_of(const type& t) -> stdx::option<const mod::module&> {
    const auto& d{t.get_data()};
    if (const auto s{d.as_opt<types::struct_t>()}) { return s->enclosing; }
    if (const auto u{d.as_opt<types::union_t>()}) { return u->enclosing; }
    if (const auto e{d.as_opt<types::enum_t>()}) { return e->enclosing; }
    if (const auto i{d.as_opt<types::interface_t>()}) { return i->enclosing; }
    return stdx::none;
}

// An impl `self` must borrow at least as strongly as the requirement.
[[nodiscard]] auto self_binding_compatible(const type& required, const type& provided) -> bool {
    const bool req_ptr{required.get_data().is<types::pointer>()};
    const bool req_ref{required.get_data().is<types::reference>()};
    const bool got_ptr{provided.get_data().is<types::pointer>()};
    const bool got_ref{provided.get_data().is<types::reference>()};

    // `&mut`/`^mut` requirement needs a mutable impl; pointer vs reference must agree; by-value
    // needs by-value.
    if (req_ptr) { return got_ptr && (required.is_constant() || !provided.is_constant()); }
    if (req_ref) { return got_ref && (required.is_constant() || !provided.is_constant()); }
    return !got_ptr && !got_ref;
}

} // namespace

auto type_resolver::visit(ast::node_id id, const ast::array_expr& array) -> void {
    PROFILE_FUNCTION();

    // `[]T` / `[N]T` with no `{ ... }`: the node is a slice/array type value
    if (array.is_type_expr) {
        resolve(array.item_explicit_type);
        auto& underlying{denoted_type(*last_type_.take())};
        if (!underlying.is_resolved()) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                incomplete_array_item(resolving_.ast.location_of(array.item_explicit_type))));
        }

        const auto mutability{array_element_mutability(array.mut_elements)};
        if (array.size) {
            gir::const_eval evaluator{ctx_, resolving_};
            const auto      len_cv{evaluator.try_eval(*array.size)};
            const auto      len{len_cv ? len_cv->as_uint_opt() : stdx::none};
            if (!len) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     "An array-type size must be a compile-time constant",
                                     error::CONSTEXPR_EVALUATION_FAILED,
                                     resolving_.ast.location_of(*array.size)));
            }
            last_type_.emplace(ctx_.get_array(
                mutability, array.null_terminated, static_cast<usize>(*len), underlying));
        } else {
            last_type_.emplace(ctx_.get_slice(mutability, array.null_terminated, underlying));
        }
        resolving_.set_sema_type(id, *last_type_);
        return;
    }

    for (const auto& item : array.items) { resolve(item); }
    resolve(array.item_explicit_type);
    auto& item_type{*last_type_.take()};

    if (!item_type.is_resolved()) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            incomplete_array_item(resolving_.ast.location_of(array.item_explicit_type))));
    }

    last_type_.emplace(ctx_.get_array(array_element_mutability(array.mut_elements),
                                      array.null_terminated,
                                      array.items.size(),
                                      item_type));
    resolving_.set_sema_type(id, *last_type_);
}

auto type_resolver::visit(ast::node_id id, const ast::asm_expr& node) -> void {
    PROFILE_FUNCTION();

    // Resolve every string literal (template, constraints, clobbers) and operand expression so
    // that all child nodes carry a resolved type for the emitter and later passes.
    resolve(node.tmpl);
    for (const auto& op : node.outputs) { resolve(op.constraint); }
    for (const auto& op : node.inputs) { resolve(op.constraint); }
    for (const auto& clobber : node.clobbers) { resolve(clobber); }
    for (const auto& op : node.inputs) {
        if (op.value) { TRY_RESOLVE(*op.value); }
    }

    usize bound_output_count{0};
    usize result_slot_count{0};
    for (const auto& op : node.outputs) {
        if (!op.value) {
            ++result_slot_count;
            continue;
        }
        {
            const mutating_context_guard g{in_mutating_context_};
            TRY_RESOLVE(*op.value);
        }
        ++bound_output_count;

        // Outputs must name something assignable; deep mutability is a pass-3 concern.
        const auto kind{(*op.value)->get_kind()};
        const auto assignable{kind == ast::node_kind::IDENTIFIER_EXPRESSION ||
                              kind == ast::node_kind::DOT_EXPRESSION ||
                              kind == ast::node_kind::INDEX_EXPRESSION ||
                              kind == ast::node_kind::DEREFERENCE_EXPRESSION ||
                              kind == ast::node_kind::IMPLICIT_ACCESS_EXPRESSION};
        if (!assignable) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 std::string{"An asm output operand must be an assignable lvalue"},
                                 error::ILLEGAL_INLINE_ASM,
                                 resolving_.ast.location_of(*op.value)));
        }
    }

    stdx::option<type&> declared_result_type;
    if (node.result_type) {
        resolve(*node.result_type);
        declared_result_type.emplace(*last_type_);
    }

    const auto fail = [&](std::string msg) -> void {
        last_type_.emplace(ctx_.poison_node(resolving_,
                                            id,
                                            std::move(msg),
                                            error::ILLEGAL_INLINE_ASM,
                                            resolving_.ast.location_of(id)));
    };

    const bool has_volatile{node.has_option(ast::asm_expr::option::VOLATILE)};
    const bool has_noreturn{node.has_option(ast::asm_expr::option::NORETURN)};
    const bool has_intel{node.has_option(ast::asm_expr::option::INTEL)};
    const bool has_att{node.has_option(ast::asm_expr::option::ATT)};

    for (const auto opt : stdx::enum_range<ast::asm_expr::option>()) {
        if (std::ranges::count(node.options, opt) > 1) { return fail("Duplicate asm option"); }
    }
    if (has_intel && has_att) {
        return fail("The asm options 'intel' and 'att' are mutually exclusive");
    }

    // `volatile` is never implied: a no-output asm must state it or it has no observable effect.
    if (node.outputs.empty() && !has_volatile) {
        return fail("An asm block with no output operands must be marked `volatile`");
    }

    if (result_slot_count > 1) {
        return fail("An asm block may have at most one `_` result operand");
    }
    if (result_slot_count == 1 && !node.result_type) {
        return fail("An asm block with a `_` result operand needs an explicit result type, "
                    "e.g. `asm u32 { ... }`");
    }
    if (result_slot_count == 1 && node.outputs.size() != 1) {
        return fail("An asm block with a `_` result operand may not bind any other outputs");
    }
    if (result_slot_count == 0 && node.result_type) {
        return fail("An asm result type is only meaningful alongside a `_` result operand");
    }
    if (has_noreturn && (bound_output_count > 0 || result_slot_count > 0)) {
        return fail("A `noreturn` asm block cannot bind any output operands");
    }

    // Template placeholder bounds: `%N` must index an operand
    const auto& tmpl_str{resolving_.ast.get_as<ast::string_expr>(node.tmpl).value};
    const usize operand_count{node.outputs.size() + node.inputs.size()};
    for (usize i{0}; i + 1 < tmpl_str.size(); ++i) {
        if (tmpl_str[i] != '%') { continue; }
        if (tmpl_str[i + 1] == '%') {
            ++i;
            continue;
        }
        if (!std::isdigit(tmpl_str[i + 1])) { continue; }
        usize idx{0};
        usize j{i + 1};
        for (; j < tmpl_str.size() && std::isdigit(tmpl_str[j]); ++j) {
            idx = (idx * 10) + static_cast<usize>(tmpl_str[j] - '0');
        }
        i = j - 1;
        if (idx >= operand_count) {
            return fail(fmt::format("asm template references operand %{} but {} operand(s) are "
                                    "given (outputs are numbered first, then inputs)",
                                    idx,
                                    operand_count));
        }
    }

    // A `_` result slot makes `asm` an rvalue of the declared type
    type& node_type{result_slot_count == 1 ? *declared_result_type
                                           : ctx_.get_builtin_resolved_type(type_kind::VOID_)};
    resolving_.set_sema_type(id, node_type);
    last_type_.emplace(node_type);
}

template <ast::IndexableID ID>
[[nodiscard]] auto type_resolver::resolve_builtin_call(ID                             id,
                                                       const ast::call_expr&          call,
                                                       const types::builtin_function& builtin)
    -> stdx::result<void, diagnostic> {
    ASSERT(call.function.is<ast::identifier_expr>(), "Builtin function must be a raw ident");
    // There must be an actual builtin present with a token id
    const auto builtin_id{call.function->get_token_type()};
    ASSERT(syntax::get_builtin_opt(builtin_id), "Cannot resolve non-builtin function");

    using syntax::token_type_t;
    const auto  is_expect_or_require{builtin_id == token_type_t::BUILTIN_EXPECT ||
                                    builtin_id == token_type_t::BUILTIN_REQUIRE};
    const auto  is_skip{builtin_id == token_type_t::BUILTIN_SKIP};
    const auto& params{builtin.params};
    if (is_expect_or_require) {
        if (call.arguments.empty() || call.arguments.size() > 2) {
            return make_sema_err(
                fmt::format("Builtin expects 1 or 2 arguments, found {}", call.arguments.size()),
                error::ARITY_MISMATCH,
                resolving_.ast.location_of(call.function));
        }
    } else if (is_skip) {
        if (call.arguments.size() > 1) {
            return make_sema_err(
                fmt::format("Builtin expects 0 or 1 arguments, found {}", call.arguments.size()),
                error::ARITY_MISMATCH,
                resolving_.ast.location_of(call.function));
        }
    } else if (call.arguments.size() != params.size()) {
        return make_sema_err(fmt::format("Builtin expects {} arguments, found {}",
                                         params.size(),
                                         call.arguments.size()),
                             error::ARITY_MISMATCH,
                             resolving_.ast.location_of(call.function));
    }

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
        // Pointer/reference/slice/array is checked since it is an invariant of the cast
        auto&      expr_type{*get_resolved_call_arg_type(call.arguments[0])};
        const auto castable_kind{expr_type.get_kind() == type_kind::POINTER ||
                                 expr_type.get_kind() == type_kind::REFERENCE ||
                                 expr_type.get_kind() == type_kind::SLICE ||
                                 expr_type.get_kind() == type_kind::ARRAY};
        if (castable_kind) {
            return_type = ctx_.pool.with_const(expr_type, false);
        } else {
            return make_sema_err(
                fmt::format("Expected pointer, reference, slice, or array type; found '{}'",
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
    // @fnCtx() returns the enclosing function's own (pre-capture) signature as a callable value
    case token_type_t::BUILTIN_FN_CTX:
        if (open_function_nodes_.empty()) {
            return make_sema_err("@fnCtx() may only be used inside of a function",
                                 error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(id));
        }
        if (auto& fn_self_type{resolving_.get_sema_type(open_function_nodes_.back())};
            fn_self_type.is_resolved()) {
            return_type = &fn_self_type;
            break;
        }
        return make_sema_err(
            "@fnCtx() needs its enclosing function's return type to be known; give it an "
            "explicit (non-auto) return type",
            error::TYPE_MISMATCH,
            resolving_.ast.location_of(id));
    case token_type_t::BUILTIN_PTR_FROM_ARRAY: {
        auto& array_type{*get_resolved_call_arg_type(call.arguments[0])};
        auto& type_data{array_type.get_data()};
        // The result pointer mirrors the array's element mutability: `[N]mut T` -> `^mut T`.
        const auto mutability{container_element_mutability(array_type)};
        if (const auto array_data{type_data.as_opt<types::array>()}) {
            return_type = &ctx_.get_pointer(mutability, array_data->underlying);
        } else if (const auto deferred_data{type_data.as_opt<types::deferred_array>()}) {
            return_type = &ctx_.get_pointer(mutability, deferred_data->underlying);
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
            // The resulting slice isn't null terminated since the pointer gives no guarantee,
            // and mirrors the source pointer's own mutability rather than being const by default
            const auto mutability{ptr_type.is_constant() ? types::mut::CONSTANT
                                                         : types::mut::MUTABLE};
            return_type = &ctx_.get_slice(mutability, false, ptr_data->underlying);
            break;
        }

        return make_sema_err(fmt::format("Expected a pointer-yielding expression; found '{}'",
                                         type_kind_display_name(ptr_type.get_kind())),
                             error::TYPE_MISMATCH,
                             get_call_arg_location(call.arguments[0]));
    }
    case token_type_t::BUILTIN_FIELD_PARENT_PTR: {
        auto& parent_type{*get_resolved_call_arg_type(call.arguments[0])};
        if (!parent_type.get_data().is<types::struct_t>()) {
            return make_sema_err(fmt::format("'@fieldParentPtr' expects a struct type; found '{}'",
                                             type_kind_display_name(parent_type.get_kind())),
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(call.arguments[0]));
        }

        const auto name_expr{call.arguments[1].as_opt<ast::expr_handle>()};
        const auto name_str{name_expr ? resolving_.ast.get_as_opt<ast::string_expr>(*name_expr)
                                      : stdx::none};
        if (!name_str) {
            return make_sema_err("'@fieldParentPtr' expects a string-literal field name",
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(call.arguments[1]));
        }
        const auto& field_table{ctx_.registry.get(parent_type.get_symbol_table_idx())};
        const auto  field_proxy{field_table.get_proxy_opt(name_str->value)};
        const auto  struct_data{parent_type.get_data().as<types::struct_t>()};
        if (!field_proxy || field_proxy->index >= struct_data.fields.size()) {
            return make_sema_err(
                fmt::format("'{}' is not a field of the given struct", name_str->value),
                error::UNKNOWN_FIELD,
                get_call_arg_location(call.arguments[1]));
        }

        auto& field_ptr_type{*get_resolved_call_arg_type(call.arguments[2])};
        if (field_ptr_type.get_kind() != type_kind::POINTER) {
            return make_sema_err(
                fmt::format("'@fieldParentPtr' expects a field pointer; found '{}'",
                            type_kind_display_name(field_ptr_type.get_kind())),
                error::TYPE_MISMATCH,
                get_call_arg_location(call.arguments[2]));
        }
        const auto mutability{field_ptr_type.is_constant() ? types::mut::CONSTANT
                                                           : types::mut::MUTABLE};
        return_type = &ctx_.get_pointer(mutability, parent_type);
        break;
    }
    case token_type_t::BUILTIN_TAG_NAME:
    case token_type_t::BUILTIN_TARGET_TRIPLE: {
        ASSERT(builtin.return_type.get_kind() == type_kind::SLICE);
        return_type = &builtin.return_type;
        break;
    }
    case token_type_t::BUILTIN_IMPLEMENTS: {
        // `@implements(T | value, I)` -> constexpr bool. The value is produced by const-eval.
        DISCARD(get_resolved_call_arg_type(call.arguments[0]));
        DISCARD(get_resolved_call_arg_type(call.arguments[1]));
        ASSERT(builtin.return_type.get_kind() == type_kind::BOOL);
        return_type = &builtin.return_type;
        break;
    }
    case token_type_t::BUILTIN_TYPE_NAME: {
        auto&      arg_type{*get_resolved_call_arg_type(call.arguments[0])};
        const auto name{ctx_.type_display_name(arg_type)};
        return_type = &ctx_.get_array(types::mut::CONSTANT,
                                      true,
                                      name.size() + 1,
                                      ctx_.get_builtin_resolved_type(type_kind::U8));
        break;
    }
    case token_type_t::BUILTIN_TARGET_OS:       return_type = &ctx_.get_builtin_type("Os"); break;
    case token_type_t::BUILTIN_TARGET_ARCH:     return_type = &ctx_.get_builtin_type("Arch"); break;
    case token_type_t::BUILTIN_TARGET_ABI:      return_type = &ctx_.get_builtin_type("Abi"); break;
    case token_type_t::BUILTIN_TARGET_FAMILY:   return_type = &ctx_.get_builtin_type("Family"); break;
    case token_type_t::BUILTIN_TARGET_ENDIAN:   return_type = &ctx_.get_builtin_type("Endian"); break;
    case token_type_t::BUILTIN_TARGET_PTR_BITS: {
        ASSERT(builtin.return_type.get_kind() == type_kind::USIZE);
        return_type = &builtin.return_type;
        break;
    }
    case token_type_t::BUILTIN_MEMCPY:
    case token_type_t::BUILTIN_MEMSET:
    case token_type_t::BUILTIN_MEMMOVE: {
        ASSERT(builtin.return_type.get_kind() == type_kind::VOID_);
        return_type = &builtin.return_type;
        break;
    }
    case token_type_t::BUILTIN_SET_EVAL_RECURSION_LIMIT: {
        if (return_trackers_.empty()) {
            return make_sema_err("@setEvalRecursionLimit can only be used within a function scope",
                                 error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(call.function));
        }
        return_type = &ctx_.get_builtin_resolved_type(type_kind::VOID_);
        break;
    }
    case token_type_t::BUILTIN_SET_MAIN_SYMBOL: {
        if (const auto expr_h{call.arguments[0].as_opt<ast::expr_handle>()}) {
            stdx::option<std::string> main_name;
            if (const auto str_expr{resolving_.ast.get_as_opt<ast::string_expr>(*expr_h)}) {
                main_name.emplace(str_expr->value);
            } else {
                gir::const_eval evaluator{ctx_, resolving_};
                if (const auto val{evaluator.try_eval(*expr_h)}) {
                    if (const auto str{val->as_opt<std::string>()}) { main_name.emplace(*str); }
                }
            }

            if (main_name) {
                if (!syntax::token_type::is_valid_identifier_name(*main_name)) {
                    return make_sema_err(
                        fmt::format(
                            "@setMainSymbol argument must be a valid identifier; found '{}'",
                            *main_name),
                        error::TYPE_MISMATCH,
                        resolving_.ast.location_of(*expr_h));
                }
                ctx_.user_main_name = *main_name;
            }
        }
        return_type = &ctx_.get_builtin_resolved_type(type_kind::VOID_);
        break;
    }
    // These return @typeOf(expression) which is trivial
    case token_type_t::BUILTIN_MUL_ADD:
    case token_type_t::BUILTIN_ABS:     {
        return_type = get_resolved_call_arg_type(call.arguments[0]);
        break;
    }
    case token_type_t::BUILTIN_MIN:
    case token_type_t::BUILTIN_MAX:
    case token_type_t::BUILTIN_DIV_TRUNC:
    case token_type_t::BUILTIN_DIV_FLOOR:
    case token_type_t::BUILTIN_REM:
    case token_type_t::BUILTIN_MOD:       {
        auto&      lhs_type{*get_resolved_call_arg_type(call.arguments[0])};
        auto&      rhs_type{*get_resolved_call_arg_type(call.arguments[1])};
        const bool floats_ok{builtin_id == token_type_t::BUILTIN_MIN ||
                             builtin_id == token_type_t::BUILTIN_MAX};
        const auto accepts{
            [&](type_kind k) -> bool { return floats_ok ? is_numeric(k) : is_integer(k); }};
        if (!accepts(lhs_type.get_kind()) || lhs_type.get_kind() != rhs_type.get_kind()) {
            return make_sema_err(
                fmt::format("'{}' expects two operands of the same {} type; found '{}' and '{}'",
                            *syntax::get_builtin_opt(builtin_id),
                            floats_ok ? "numeric" : "integer",
                            type_kind_display_name(lhs_type.get_kind()),
                            type_kind_display_name(rhs_type.get_kind())),
                error::OPERATOR_TYPE_MISMATCH,
                get_call_arg_location(call.arguments[0]));
        }
        return_type = ctx_.pool.with_const(lhs_type, false);
        break;
    }
    case token_type_t::BUILTIN_ADD_WITH_OVERFLOW:
    case token_type_t::BUILTIN_SUB_WITH_OVERFLOW:
    case token_type_t::BUILTIN_MUL_WITH_OVERFLOW:
    case token_type_t::BUILTIN_SHL_WITH_OVERFLOW: {
        auto& lhs_type{*get_resolved_call_arg_type(call.arguments[0])};
        auto& rhs_type{*get_resolved_call_arg_type(call.arguments[1])};
        auto& out_type{*get_resolved_call_arg_type(call.arguments[2])};
        if (!is_integer(lhs_type.get_kind()) || lhs_type.get_kind() != rhs_type.get_kind()) {
            return make_sema_err(
                fmt::format("'{}' expects two integer operands of the same type; found '{}' and "
                            "'{}'",
                            *syntax::get_builtin_opt(builtin_id),
                            type_kind_display_name(lhs_type.get_kind()),
                            type_kind_display_name(rhs_type.get_kind())),
                error::OPERATOR_TYPE_MISMATCH,
                get_call_arg_location(call.arguments[0]));
        }
        // The result slot is a mutable reference (`&mut T`); a `^mut T` pointer is also accepted.
        const auto  out_ref{out_type.get_data().as_opt<types::reference>()};
        const auto  out_ptr{out_type.get_data().as_opt<types::pointer>()};
        const type* out_underlying{out_ref   ? &out_ref->underlying
                                   : out_ptr ? &out_ptr->underlying
                                             : nullptr};
        if (!out_underlying || out_type.is_constant() ||
            out_underlying->get_kind() != lhs_type.get_kind()) {
            return make_sema_err(
                fmt::format("'{}' expects its third argument to be a '&mut {}' result reference; "
                            "found '{}'",
                            *syntax::get_builtin_opt(builtin_id),
                            type_kind_display_name(lhs_type.get_kind()),
                            out_type.to_string()),
                error::TYPE_MISMATCH,
                get_call_arg_location(call.arguments[2]));
        }
        return_type = &ctx_.get_builtin_resolved_type(type_kind::BOOL);
        break;
    }
    case token_type_t::BUILTIN_C_VA_START:
    case token_type_t::BUILTIN_C_VA_COPY:
    case token_type_t::BUILTIN_C_VA_END:   {
        ASSERT(builtin.return_type.get_kind() == type_kind::VOID_);
        return_type = &builtin.return_type;
        break;
    }
    case token_type_t::BUILTIN_C_VA_ARG: {
        return_type = get_resolved_call_arg_type(call.arguments[1]);
        break;
    }
    case token_type_t::BUILTIN_PANIC: {
        ASSERT(builtin.return_type.get_kind() == type_kind::NORETURN);
        // The message must be compile-time known so it can be validated / interned
        if (!call.arguments.empty()) {
            bool is_const_string{false};
            if (const auto expr_h{call.arguments[0].as_opt<ast::expr_handle>()}) {
                if (resolving_.ast.get_as_opt<ast::string_expr>(*expr_h)) {
                    is_const_string = true;
                } else {
                    gir::const_eval evaluator{ctx_, resolving_};
                    if (const auto val{evaluator.try_eval(*expr_h)}) {
                        is_const_string = val->is<std::string>();
                    }
                }
            }
            if (!is_const_string) {
                return make_sema_err("@panic message must be a compile-time-constant string",
                                     error::CONSTEXPR_EVALUATION_FAILED,
                                     get_call_arg_location(call.arguments[0]));
            }
        }
        return_type = &builtin.return_type;
        break;
    }
    case token_type_t::BUILTIN_TRAP: {
        ASSERT(builtin.return_type.get_kind() == type_kind::NORETURN);
        return_type = &builtin.return_type;
        break;
    }
    case token_type_t::BUILTIN_SKIP: {
        ASSERT(builtin.return_type.get_kind() == type_kind::NORETURN);
        // An optional message must be compile-time known so it can be interned
        if (!call.arguments.empty()) {
            bool is_const_string{false};
            if (const auto expr_h{call.arguments[0].as_opt<ast::expr_handle>()}) {
                if (resolving_.ast.get_as_opt<ast::string_expr>(*expr_h)) {
                    is_const_string = true;
                } else {
                    gir::const_eval evaluator{ctx_, resolving_};
                    if (const auto val{evaluator.try_eval(*expr_h)}) {
                        is_const_string = val->is<std::string>();
                    }
                }
            }
            if (!is_const_string) {
                return make_sema_err("@skip message must be a compile-time-constant string",
                                     error::CONSTEXPR_EVALUATION_FAILED,
                                     get_call_arg_location(call.arguments[0]));
            }
        }
        return_type = &builtin.return_type;
        break;
    }
    case token_type_t::BUILTIN_COMPILE_ERROR: {
        std::string message{"compilation aborted by @compileError"};
        if (!call.arguments.empty()) {
            if (const auto expr_h{call.arguments[0].as_opt<ast::expr_handle>()}) {
                if (const auto str_expr{resolving_.ast.get_as_opt<ast::string_expr>(*expr_h)}) {
                    message = str_expr->value;
                } else {
                    gir::const_eval evaluator{ctx_, resolving_};
                    if (const auto val{evaluator.try_eval(*expr_h)}) {
                        if (const auto str{val->as_opt<std::string>()}) { message = *str; }
                    }
                }
            }
        }
        ctx_.diags.emplace_back(std::move(message),
                                error::COMPILE_ERROR_REACHED,
                                resolving_.ast.location_of(call.function));
        return_type = &builtin.return_type;
        break;
    }
    case token_type_t::BUILTIN_SRC: {
        return_type = &ctx_.get_builtin_type("SourceLocation");
        break;
    }
    case token_type_t::BUILTIN_EXPECT:
    case token_type_t::BUILTIN_REQUIRE: {
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

auto type_resolver::local_const_fn_ref(ast::expr_handle expr) -> stdx::option<gir::const_value> {
    const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(expr)};
    if (!ident) { return stdx::none; }
    const auto sym{ctx_.registry.lookup(table_stack_, ident->name)};
    if (!sym) { return stdx::none; }
    const auto node{sym->get_data().as_opt<symbols::node_t>()};
    if (!node) { return stdx::none; }
    const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*node)};
    if (!decl || !decl->value ||
        !(decl->has_modifier(ast::decl_modifiers::CONSTANT) ||
          decl->has_modifier(ast::decl_modifiers::CONSTEXPR))) {
        return stdx::none;
    }
    if (!resolving_.ast.get_as_opt<ast::function_expr>(*decl->value)) { return stdx::none; }
    const auto fn_type{resolving_.get_sema_type_opt(*decl->value)};
    if (!fn_type || fn_type->get_kind() != type_kind::FUNCTION) { return stdx::none; }
    // A capture-less closure value gets emitted as a plain function
    return gir::const_value{gir::const_value::data_t{
                                gir::const_closure{
                                    .fn_node  = *decl->value,
                                    .module   = &resolving_,
                                    .captures = {},
                                },
                            },
                            *fn_type};
}

auto type_resolver::constexpr_closure_value(ast::expr_handle expr)
    -> stdx::option<gir::const_value> {
    // Resolve `expr` to the `function_expr` node: either a literal or a `const` local bound to one.
    ast::node_id fn_node{ast::node_id::make_invalid()};
    if (resolving_.ast.get_as_opt<ast::function_expr>(expr)) {
        fn_node = *expr;
    } else if (const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(expr)}) {
        const auto sym{ctx_.registry.lookup(table_stack_, ident->name)};
        if (!sym) { return stdx::none; }
        const auto node{sym->get_data().as_opt<symbols::node_t>()};
        if (!node) { return stdx::none; }
        const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*node)};
        if (!decl || !decl->value ||
            !(decl->has_modifier(ast::decl_modifiers::CONSTANT) ||
              decl->has_modifier(ast::decl_modifiers::CONSTEXPR)) ||
            !resolving_.ast.get_as_opt<ast::function_expr>(*decl->value)) {
            return stdx::none;
        }
        fn_node = *decl->value;
    } else {
        return stdx::none;
    }

    const auto cl_type{resolving_.get_sema_type_opt(fn_node)};
    if (!cl_type || cl_type->get_kind() != type_kind::CLOSURE) { return stdx::none; }
    const auto cl_data{cl_type->get_data().as_opt<types::closure_t>()};
    if (!cl_data) { return stdx::none; }

    gir::const_closure result{
        .fn_node  = fn_node,
        .module   = &resolving_,
        .captures = {},
    };

    for (const auto& capture : cl_data->captures) {
        const auto sym{ctx_.registry.lookup(table_stack_, capture.name)};
        if (!sym) { return stdx::none; }
        const auto node{sym->get_data().as_opt<symbols::node_t>()};
        if (!node) { return stdx::none; }
        const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*node)};
        if (!decl || !decl->value ||
            !(decl->has_modifier(ast::decl_modifiers::CONSTANT) ||
              decl->has_modifier(ast::decl_modifiers::CONSTEXPR))) {
            return stdx::none;
        }
        gir::const_eval evaluator{ctx_, resolving_};
        auto            cv{evaluator.try_eval(*decl->value)};
        if (!cv || cv->is_poison()) { return stdx::none; }
        result.captures.fields.emplace(std::string{capture.name}, std::move(*cv));
    }

    return gir::const_value{gir::const_value::data_t{std::move(result)}, *cl_type};
}

namespace {

[[nodiscard]] auto any_param_constexpr(const ast::function_expr& fn) noexcept -> bool {
    return std::ranges::any_of(fn.parameters, [](const auto& p) { return p.is_constexpr; });
}

[[nodiscard]] auto is_generic_type(const type& t) noexcept -> bool {
    const auto kind{t.get_kind()};
    if (kind == type_kind::AUTO) { return true; }

    const auto& data{t.get_data()};
    if (const auto deferred{data.as_opt<types::deferred_array>()}) {
        return is_generic_type(deferred->underlying);
    }
    if (kind == type_kind::TYPE) { return true; }

    if (const auto ptr{data.as_opt<types::pointer>()}) { return is_generic_type(ptr->underlying); }
    if (const auto ref{data.as_opt<types::reference>()}) {
        return is_generic_type(ref->underlying);
    }
    if (const auto slice{data.as_opt<types::slice>()}) {
        return is_generic_type(slice->underlying);
    }
    if (const auto arr{data.as_opt<types::array>()}) { return is_generic_type(arr->underlying); }

    // A fn-typed slot may bind either a plain function or a capturing closure at any call
    if (kind == type_kind::FUNCTION) { return true; }
    return false;
}

// A closure is only unsound to let escape its defining frame if it holds a ref into that frame
[[nodiscard]] auto has_dangling_capture(const types::closure_t& cl) noexcept -> bool {
    return std::ranges::any_of(cl.captures, [](const types::closure_capture& capture) {
        return capture.mode != types::capture_mode::VALUE;
    });
}

[[nodiscard]] auto any_param_generic(auto&& params) noexcept -> bool {
    for (const auto* p : params) {
        if (is_generic_type(*p)) { return true; }
    }
    return false;
}

// Returns `t` with every occurrence of `from` replaced by `to`, looking through pointer,
// reference, and function types. Returns `t` unchanged (same object) when nothing matched.
[[nodiscard]] auto remap_type(context& ctx, type& t, const type& from, type& to) -> type& {
    if (&t == &from) { return to; }
    return t.get_data().visit(
        [&](types::pointer p) -> type& {
            auto& u{remap_type(ctx, p.underlying, from, to)};
            if (&u == &p.underlying) { return t; }
            return ctx.get_pointer(t.is_constant() ? types::mut::CONSTANT : types::mut::MUTABLE, u);
        },
        [&](types::reference r) -> type& {
            auto& u{remap_type(ctx, r.underlying, from, to)};
            if (&u == &r.underlying) { return t; }
            return ctx.get_reference(t.is_constant() ? types::mut::CONSTANT : types::mut::MUTABLE,
                                     u);
        },
        [&](types::slice sl) -> type& {
            auto& u{remap_type(ctx, sl.underlying, from, to)};
            if (&u == &sl.underlying) { return t; }
            return ctx.get_slice(t.is_constant() ? types::mut::CONSTANT : types::mut::MUTABLE,
                                 sl.null_terminated,
                                 u);
        },
        [&](types::array ar) -> type& {
            auto& u{remap_type(ctx, ar.underlying, from, to)};
            if (&u == &ar.underlying) { return t; }
            return ctx.get_array(t.is_constant() ? types::mut::CONSTANT : types::mut::MUTABLE,
                                 ar.null_terminated,
                                 ar.len,
                                 u);
        },
        [&](types::deferred_array da) -> type& {
            auto& u{remap_type(ctx, da.underlying, from, to)};
            if (&u == &da.underlying) { return t; }
            auto& nt{*ctx.pool[{type_kind::TYPE, t.get_key().get_mut(), &da.array, &u}]};
            nt.resolve_if<types::deferred_array>(da.array, u);
            return nt;
        },
        [&](types::function fn) -> type& {
            bool changed{false};
            auto new_params{ctx.pool.get_many_unsafe(fn.params.size())};
            for (usize i{0}; i < fn.params.size(); ++i) {
                auto& np{remap_type(ctx, *fn.params[i], from, to)};
                new_params[i] = &np;
                changed |= &np != fn.params[i];
            }
            auto& new_ret{remap_type(ctx, fn.return_type, from, to)};
            if (!changed && &new_ret == &fn.return_type) { return t; }

            types::key_t key{type_kind::FUNCTION, t.get_key().get_mut()};
            for (auto* p : new_params) { key.imprint(*p); }
            key.imprint(new_ret);
            key.imprint(static_cast<u64>(fn.has_self));
            key.imprint(static_cast<u64>(fn.is_variadic));
            auto& nt{*ctx.pool[key]};
            nt.resolve_if<types::function>(new_params, new_ret, fn.has_self, fn.is_variadic);
            return nt;
        },
        [&t](const auto&) -> type& { return t; });
}

// The node id of the struct/union/enum literal a `fn(...): type` body returns, if any.
[[nodiscard]] auto returned_aggregate_node(const ast::AST& ast, const ast::block_stmt& block)
    -> stdx::option<ast::node_id> {
    for (const auto& stmt : block) {
        const auto ret{ast.get_as_opt<ast::return_stmt>(stmt)};
        if (!ret || !ret->expression) { continue; }
        const ast::node_id expr{*ret->expression};
        if (expr.any<ast::struct_expr, ast::union_expr, ast::enum_expr>()) { return expr; }
    }
    return stdx::none;
}

[[nodiscard]] auto aggregate_member_list(const ast::AST& ast, ast::node_id agg)
    -> stdx::option<const ast::member_list&> {
    if (const auto s{ast.get_as_opt<ast::struct_expr>(agg)}) { return s->members; }
    if (const auto u{ast.get_as_opt<ast::union_expr>(agg)}) { return u->members; }
    if (const auto e{ast.get_as_opt<ast::enum_expr>(agg)}) { return e->members; }
    return stdx::none;
}

// Records one `context::type_ctor_member_emit` per `const m := fn ...` member of the aggregate
// returned by a `fn(...): type` constructor.
auto register_type_ctor_members(context&         ctx,
                                mod::module&     fn_mod,
                                ast::node_id     agg_node,
                                type&            src_agg,
                                type&            clone,
                                std::string_view ctor_mangled,
                                body_type_diff   typing) -> void {
    const auto members{aggregate_member_list(fn_mod.ast, agg_node)};
    if (!members) { return; }

    bool any_fn_member{false};
    for (const auto& m : *members) {
        const auto decl{fn_mod.ast.get_as_opt<ast::decl_stmt>(*m)};
        if (!decl || !decl->value) { continue; }
        if (!fn_mod.ast.get_as_opt<ast::function_expr>(*decl->value)) { continue; }
        const auto& name{fn_mod.ast.get_as<ast::identifier_expr>(decl->name).name};
        fn_mod.type_ctor_member_emits.emplace_back<type_ctor_member_emit>({
            .owner_clone = &clone,
            .member_decl = *m,
            .gir_name    = fmt::format("{}.{}", ctor_mangled, name),
            .typing_key  = std::string{ctor_mangled},
        });
        any_fn_member = true;
    }
    if (!any_fn_member) { return; }

    // The replay must place `@this()` / `.{ ... }` / `^self` nodes at `clone`, not the shared
    // literal type that later instantiations overwrite.
    for (auto& [_, ty] : typing.node_types) {
        if (ty) { ty.emplace(remap_type(ctx, *ty, src_agg, clone)); }
    }
    for (auto& [_, ty] : typing.explicit_types) {
        if (ty) { ty.emplace(remap_type(ctx, *ty, src_agg, clone)); }
    }
    if (!typing.empty()) {
        ctx.instantiation_cache.set_body_type_diff(std::string{ctor_mangled}, std::move(typing));
    }
    ctx.generic_functions.set_type_ctor_member_prefix(clone, std::string{ctor_mangled});
}

// Copies an anonymous aggregate `sema::type` into a fresh pool entry, keyed by `disc` so repeated
// requests for the same instantiation share one type
[[nodiscard]] auto clone_anonymous_aggregate(context& ctx, type& src, std::string_view disc)
    -> type& {
    types::key_t key{src.get_kind(), src.get_key().get_mut()};
    key.imprint(disc);
    auto& fresh{*ctx.pool[key]};
    if (fresh.is_resolved()) { return fresh; }
    if (src.has_symbol_table_idx()) { fresh.set_symbol_table_idx(src.get_symbol_table_idx()); }
    ctx.generic_functions.set_clone_disc(fresh, std::string{disc});

    // Rebind so distinct instantiations of a `fn(T): type` ctor do not alias each other's shape
    const auto rebind{[&](gsl::span<type*> types) -> gsl::span<type*> {
        auto out{ctx.pool.get_many_unsafe(types.size())};
        for (usize i{0}; i < types.size(); ++i) {
            out[i] = &remap_type(ctx, *types[i], src, fresh);
        }
        return out;
    }};

    src.get_data().visit(
        [](const auto&) { UNREACHABLE("clone_anonymous_aggregate on a non-aggregate type"); },
        [&](const types::struct_t& s) {
            fresh.resolve<types::struct_t>(rebind(s.fields),
                                           s.ast_fields,
                                           rebind(s.members),
                                           s.enclosing,
                                           s.is_c_abi,
                                           s.is_packed,
                                           s.field_alignments);
        },
        [&](const types::union_t& u) {
            fresh.resolve<types::union_t>(
                rebind(u.fields), u.ast_fields, rebind(u.members), u.enclosing, u.is_untagged);
        },
        [&](const types::enum_t& e) {
            fresh.resolve<types::enum_t>(
                e.ast_enumerations, e.non_exhaustive, e.underlying, rebind(e.members), e.enclosing);
        });
    return fresh;
}

// Replaces characters that are meaningful in a discriminator but undesirable in a symbol name.
[[nodiscard]] auto sanitize_mangled(std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c == '#' || c == '.' || c == ':') {
            out += '_';
        } else {
            out += c;
        }
    }
    return out;
}

// Mangles a type such that it will not conflict with any other monomorphized instance
[[nodiscard]] auto mangle_arg_type(const generic_function_registry& reg, const type& t)
    -> std::string {
    if (const auto meta{t.get_data().as_opt<types::meta_type>()}) {
        return "type_" + mangle_arg_type(reg, meta->instance);
    }
    const auto& data{t.get_data()};
    switch (t.get_kind()) {
    case type_kind::STRUCT:
    case type_kind::UNION:
    case type_kind::ENUM:
    case type_kind::CLOSURE: {
        const auto kind_name{type_kind_display_name(t.get_kind())};
        if (const auto disc{reg.get_clone_disc(t)}) {
            return fmt::format("{}${}", kind_name, sanitize_mangled(*disc));
        }
        if (t.has_symbol_table_idx()) {
            return fmt::format("{}{}", kind_name, t.get_symbol_table_idx());
        }
        return std::string{kind_name};
    }
    case type_kind::FUNCTION: {
        const auto& fn{data.as<types::function>()};
        return fmt::format("fn_{}__{}",
                           fmt::join(fn.params | std::views::transform([&](const auto* p) {
                                         return mangle_arg_type(reg, *p);
                                     }),
                                     "_"),
                           mangle_arg_type(reg, fn.return_type));
    }
    case type_kind::ARRAY: {
        const auto& arr{data.as<types::array>()};
        return fmt::format("array{}_{}", arr.len, mangle_arg_type(reg, arr.underlying));
    }
    case type_kind::SLICE: {
        const auto& sl{data.as<types::slice>()};
        return fmt::format("slice_{}", mangle_arg_type(reg, sl.underlying));
    }
    case type_kind::POINTER: {
        const auto& p{data.as<types::pointer>()};
        return fmt::format("ptr_{}", mangle_arg_type(reg, p.underlying));
    }
    case type_kind::REFERENCE: {
        const auto& r{data.as<types::reference>()};
        return fmt::format("ref_{}", mangle_arg_type(reg, r.underlying));
    }
    default: return std::string{type_kind_display_name(t.get_kind())};
    }
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

    // A call whose callee is still generic (e.g. `w.write(...)` where `w: &impl I` desugared to
    // `&auto`) only resolves once the enclosing generic is instantiated.
    if (callee_type.get_kind() == type_kind::AUTO) {
        resolve_call_args(call.arguments);
        resolving_.set_sema_type(id, callee_type);
        return last_type_.emplace(callee_type);
    }

    if constexpr (std::same_as<ID, ast::node_id>) {
        record_type_ctor_member_call(id, call);

        // A `.`-method belonging to a parameterized-impl expansion is emitted under a
        // per-instantiation symbol
        if (pending_param_impl_target_) {
            resolving_.set_generic_call_target(id, std::move(*pending_param_impl_target_));
            pending_param_impl_target_.reset();
        }
    }

    // Verify that the type in the function is callable and store the return type
    auto&                                callee_data{callee_type.get_data()};
    stdx::option<const types::function&> function_type;
    bool                                 is_closure_call{false};
    if (const auto ft{callee_data.as_opt<types::function>()}) {
        function_type = ft;
    } else if (const auto ptr{callee_data.as_opt<types::pointer>()}) {
        function_type = ptr->underlying.get_data().as_opt<types::function>();
    } else if (const auto ref{callee_data.as_opt<types::reference>()}) {
        function_type = ref->underlying.get_data().as_opt<types::function>();
    } else if (const auto cl{callee_data.as_opt<types::closure_t>()}) {
        // Called directly since synthetic `&mut self` param is always implicit here
        function_type   = cl->impl_signature.get_data().as_opt<types::function>();
        is_closure_call = true;
    }

    if (function_type) {
        const auto dot_call{resolving_.ast.get_as_opt<ast::dot_expr>(call.function)};
        bool       is_obj_instance{false};
        if (dot_call) {
            bool       is_type{false};
            const auto target_obj{dot_call->object};
            if (const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(target_obj)}) {
                if (const auto sym{ctx_.registry.lookup(table_stack_, ident->name)}) {
                    if (sym->has_kind() && sym->get_kind() == symbol_kind::TYPE) { is_type = true; }
                }
            } else if (const auto mac{
                           resolving_.ast.get_as_opt<ast::module_access_expr>(target_obj)}) {
                if (const auto mod_type{resolving_.get_sema_type_opt(mac->outer)}) {
                    if (const auto m_data{mod_type->get_data().as_opt<types::module>()}) {
                        const auto& inner_mod{m_data->imported};
                        if (inner_mod.root_table_idx) {
                            const auto& inner_ident{
                                resolving_.ast.get_as<ast::identifier_expr>(mac->inner)};
                            if (const auto sym{ctx_.registry.get_from_opt(*inner_mod.root_table_idx,
                                                                          inner_ident.name)}) {
                                if (sym->has_kind() && sym->get_kind() == symbol_kind::TYPE) {
                                    is_type = true;
                                }
                            }
                        }
                    }
                }
            }

            if (!is_type) { is_obj_instance = true; }
        }
        const auto has_implicit_self{is_closure_call ||
                                     (function_type->has_self && is_obj_instance)};

        // Check the arity of the function against params before resetting last type
        const auto& params{function_type->params};
        const usize param_offset{has_implicit_self ? 1UZ : 0UZ};
        const auto  expected_arity{params.size() - param_offset};
        if (function_type->is_variadic) {
            if (call.arguments.size() < expected_arity) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     fmt::format("Expected at least {} arguments, found {}",
                                                 expected_arity,
                                                 call.arguments.size()),
                                     error::ARITY_MISMATCH,
                                     resolving_.ast.location_of(call.function)));
            }
        } else if (call.arguments.size() != expected_arity) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format(
                    "Expected {} arguments, found {}", expected_arity, call.arguments.size()),
                error::ARITY_MISMATCH,
                resolving_.ast.location_of(call.function)));
        }

        const auto fn_info_opt{ctx_.generic_functions.get_opt(callee_type)};
        if (fn_info_opt &&
            (any_param_generic(params) || any_param_constexpr(*fn_info_opt->fn_expr))) {
            auto concrete_arg_types{ctx_.pool.get_many_unsafe(call.arguments.size())};
            bool any_arg_poison{false};
            for (usize i{0}; auto [param_type, arg] :
                             std::views::zip(params.subspan(param_offset), call.arguments)) {
                stdx::option<structural_guard> g;
                if (!is_generic_type(*param_type)) { g.emplace(implicit_type_stack_, *param_type); }

                auto result_arg_type =
                    arg.visit([this, param_type](auto arg_id) -> stdx::option<type&> {
                        if (param_type->get_kind() == type_kind::TYPE) {
                            if (const auto ident{
                                    resolving_.ast.get_as_opt<ast::identifier_expr>(arg_id)}) {
                                if (auto sym{ctx_.registry.lookup(table_stack_, ident->name)}) {
                                    if (auto b{sym.value()
                                                   .get_data()
                                                   .template as_opt<symbols::builtin>()}) {
                                        return b.value().get_type();
                                    }
                                    if (auto node{sym.value()
                                                      .get_data()
                                                      .template as_opt<symbols::node_t>()}) {
                                        if (resolving_.has_sema_type(*node)) {
                                            return resolving_.get_sema_type(*node);
                                        }
                                    }
                                }
                            }
                        }
                        resolve(arg_id);
                        auto* arg_type{last_type_.take()};
                        if (arg_type->is_poison()) { return stdx::none; }
                        // `@typeOf(x)` in a `type` argument position denotes the type it wraps.
                        if (param_type->get_kind() == type_kind::TYPE) {
                            return denoted_type(*arg_type);
                        }
                        return arg_type;
                    });
                if (!result_arg_type) { any_arg_poison = true; }
                concrete_arg_types[i++] = result_arg_type.take();
            }
            if (any_arg_poison) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }

            // An argument that still contains `auto` (`@typeOf(a)` where `a: auto`) can only be
            // resolved once the enclosing generic is instantiated
            const auto arg_contains_auto{[](auto&& self, const type& t) -> bool {
                const auto& d{denoted_type(const_cast<type&>(t))};
                if (d.get_kind() == type_kind::AUTO) { return true; }
                const auto& data{d.get_data()};
                if (const auto p{data.template as_opt<types::pointer>()}) {
                    return self(self, p->underlying);
                }
                if (const auto r{data.template as_opt<types::reference>()}) {
                    return self(self, r->underlying);
                }
                if (const auto sl{data.template as_opt<types::slice>()}) {
                    return self(self, sl->underlying);
                }
                if (const auto ar{data.template as_opt<types::array>()}) {
                    return self(self, ar->underlying);
                }
                return false;
            }};
            if (std::ranges::any_of(concrete_arg_types, [&](type* t) {
                    return arg_contains_auto(arg_contains_auto, *t);
                })) {
                auto& placeholder{*ctx_.pool[{type_kind::AUTO, types::mut::CONSTANT}]};
                resolving_.set_sema_type(id, placeholder);
                return last_type_.emplace(placeholder);
            }

            // Enforce `impl I` / `impl (A + B)` parameter bounds now the arguments are concrete.
            if (const auto it{impl_param_bounds_.find(&callee_type)};
                it != impl_param_bounds_.end()) {
                for (const auto& [pidx, ifaces] : it->second) {
                    if (pidx >= concrete_arg_types.size()) { continue; }
                    gsl::not_null bound_t{&denoted_type(*concrete_arg_types[pidx])};
                    if (const auto p{bound_t->get_data().as_opt<types::pointer>()}) {
                        bound_t = &p->underlying;
                    } else if (const auto r{bound_t->get_data().as_opt<types::reference>()}) {
                        bound_t = &r->underlying;
                    }

                    for (const auto* iface : ifaces) {
                        if (ctx_.impls.implements(*bound_t, *iface)) { continue; }
                        const auto& pname{resolving_.ast.get_as<ast::identifier_expr>(
                            *fn_info_opt->fn_expr->parameters[pidx].name)};
                        return last_type_.emplace(ctx_.poison_node(
                            resolving_,
                            id,
                            fmt::format("`{}` does not implement `{}` required by parameter `{}`",
                                        ctx_.type_display_name(*bound_t),
                                        ctx_.type_display_name(*iface),
                                        pname.name),
                            error::UNSATISFIED_BOUND,
                            get_call_arg_location(call.arguments[pidx])));
                    }
                }
            }

            // Fold the argument supplied for each `constexpr` parameter to a compile-time value
            const auto& cx_params{fn_info_opt->fn_expr->parameters};
            const auto  cx_count{static_cast<usize>(
                std::ranges::count_if(cx_params, [](const auto& p) { return p.is_constexpr; }))};
            auto        constexpr_args{ctx_.arena.make_span<gir::const_value>(cx_count)};
            for (usize i{0}, cx_i{0}; i < cx_params.size() && i < call.arguments.size(); ++i) {
                if (!cx_params[i].is_constexpr) { continue; }
                stdx::option<gir::const_value> folded;
                if (const auto expr_h{call.arguments[i].as_opt<ast::expr_handle>()}) {
                    gir::const_eval evaluator{ctx_, resolving_};
                    if (auto cv{evaluator.try_eval(*expr_h)}; cv && !cv->is_poison()) {
                        folded.emplace(std::move(*cv));
                    } else if (auto ref{local_const_fn_ref(*expr_h)}) {
                        folded.emplace(std::move(*ref));
                    } else if (auto clv{constexpr_closure_value(*expr_h)}) {
                        folded.emplace(std::move(*clv));
                    }
                }

                if (!folded) {
                    const auto* arg_ty{concrete_arg_types[i]};
                    const auto  msg{arg_ty && arg_ty->get_kind() == type_kind::CLOSURE
                                        ? "a constexpr closure argument must capture only "
                                          "compile-time constants"
                                        : "argument to a constexpr parameter must be a "
                                          "compile-time constant"};
                    return last_type_.emplace(
                        ctx_.poison_node(resolving_,
                                         id,
                                         msg,
                                         error::CONSTEXPR_EVALUATION_FAILED,
                                         get_call_arg_location(call.arguments[i])));
                }
                constexpr_args[cx_i++] = std::move(*folded);
            }

            generic_instantiation_key key{
                .generic_fn_type = &callee_type,
                .arg_types       = concrete_arg_types,
                .constexpr_args  = constexpr_args,
            };

            if (const auto cached{ctx_.instantiation_cache.find(key)}) {
                resolving_.set_generic_call_target(id, cached->mangled_name);
                resolving_.set_sema_type(id, *cached->return_type);
                return last_type_.emplace(*cached->return_type);
            }

            auto inst_res{
                instantiate_generic(callee_type, *fn_info_opt, concrete_arg_types, constexpr_args)};
            if (!inst_res) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }

            auto& return_type{*inst_res->return_type};
            auto  mangled_name{inst_res->mangled_name};
            ctx_.instantiation_cache.insert(std::move(key), return_type, mangled_name);
            resolving_.set_generic_call_target(id, mangled_name);
            resolving_.set_sema_type(id, return_type);
            return last_type_.emplace(return_type);
        }

        bool any_arg_poison{false};
        for (usize i{0}; const auto& arg : call.arguments) {
            stdx::option<structural_guard> g;
            if (i < expected_arity) {
                g.emplace(implicit_type_stack_, *function_type->params[i + param_offset]);
            }
            any_arg_poison |= arg.visit([this](auto arg_id) -> bool {
                resolve(arg_id);
                return last_type_.take()->is_poison();
            });
            ++i;
        }
        if (any_arg_poison) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }

        // Functions that return a type cannot be resolved until the constant evaluator
        if (function_type->return_type.get_kind() == type_kind::TYPE &&
            !any_param_generic(function_type->params)) {
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

namespace {

// Identifiers, fields, elements, or deref off one have  real storage; anything else is an rvalue.
[[nodiscard]] auto is_lvalue_shape(const mod::module& module, ast::expr_handle expr) noexcept
    -> bool {
    return module.ast.get_as_opt<ast::identifier_expr>(expr) ||
           module.ast.get_as_opt<ast::dot_expr>(expr) ||
           module.ast.get_as_opt<ast::index_expr>(expr) ||
           module.ast.get_as_opt<ast::dereference_expr>(expr);
}

// Applies a `&`/`&mut`/`^`/`^mut`/none capture modifier, rejecting const or address-of-rvalue.
[[nodiscard]] auto resolve_capture_modifier(context&           ctx,
                                            ast::type_modifier modifier,
                                            type&              base_type,
                                            bool               container_is_const,
                                            bool               container_is_addressable,
                                            std::string_view   what,
                                            source_location    loc) noexcept
    -> stdx::result<gsl::not_null<type*>, diagnostic> {
    const bool wants_address{modifier.is_ref() || modifier.is_ptr()};
    if (wants_address && !container_is_addressable) {
        return make_sema_err(
            fmt::format("Cannot capture a temporary {} by {}; only a plain value capture is "
                        "allowed here since it has no storage of its own",
                        what,
                        modifier.is_ptr() ? "pointer" : "reference"),
            error::ILLEGAL_RVALUE_CAPTURE,
            loc);
    }

    const bool wants_mutable{modifier.is_mutable_ref() || modifier.is_mutable_ptr()};
    if (wants_mutable && container_is_const) {
        return make_sema_err(fmt::format("Cannot capture an immutable {} by mutable {}",
                                         what,
                                         modifier.is_mutable_ref() ? "reference" : "pointer"),
                             error::ASSIGNMENT_TO_CONST,
                             loc);
    }

    if (modifier.is_ref()) {
        return gsl::not_null{&ctx.get_reference(
            modifier.is_mutable_ref() ? types::mut::MUTABLE : types::mut::CONSTANT, base_type)};
    }
    if (modifier.is_ptr()) {
        return gsl::not_null{&ctx.get_pointer(
            modifier.is_mutable_ptr() ? types::mut::MUTABLE : types::mut::CONSTANT, base_type)};
    }
    return gsl::not_null{&base_type};
}

} // namespace

auto type_resolver::visit(ast::node_id id, const ast::for_loop_expr& for_expr) -> void {
    PROFILE_FUNCTION();
    ASSERT(for_expr.iterables.size() == for_expr.captures.size());

    // The loop itself holds the block index which houses captures, not the block
    auto& loop_type{resolving_.get_sema_type(id)};
    {
        const scope s{table_stack_, loop_type.get_symbol_table_idx(), table_idx_};

        // A `for (arr, lo..) |v, i|` open-upper range needs a sibling array/slice to stop the loop.
        bool has_open_upper_range{false};
        bool has_bounding_iterable{false};
        for (const auto& iterable : for_expr.iterables) {
            const auto rng{resolving_.ast.get_as_opt<ast::range_expr>(iterable)};
            if (rng && !rng->rhs) {
                has_open_upper_range = true;
            } else {
                has_bounding_iterable = true;
            }
        }
        if (has_open_upper_range && !has_bounding_iterable) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                "An open-ended range loop needs an array or slice iterable to bound it, e.g. "
                "`for (arr, 0..) |v, i|`",
                error::ILLEGAL_OPEN_RANGE,
                resolving_.ast.location_of(id)));
        }

        // The captures must be paired with the iterables inner types (shallow type check)
        for (const auto& [capture, iterable] :
             std::views::zip(for_expr.captures, for_expr.iterables)) {
            {
                const mutating_context_guard for_iter_g{in_for_iterable_, true};
                TRY_RESOLVE(iterable);
            }
            auto& iterable_type{*last_type_.take()};
            resolving_.set_sema_type(iterable, iterable_type);

            // Assign types unconditionally since ignoring discards saves no space
            auto& iterable_data{iterable_type.get_data()};
            type* elem_type{nullptr};
            if (const auto array{iterable_data.as_opt<types::array>()}) {
                elem_type = &array->underlying;
            } else if (const auto deferred{iterable_data.as_opt<types::deferred_array>()}) {
                elem_type = &deferred->underlying;
            } else if (const auto slice{iterable_data.as_opt<types::slice>()}) {
                elem_type = &slice->underlying;
            } else {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    fmt::format("Iterables may only be arrays or slices; found '{}'",
                                type_kind_display_name(iterable_type.get_kind())),
                    error::TYPE_MISMATCH,
                    resolving_.ast.location_of(iterable)));
            }

            auto cap_result{resolve_capture_modifier(ctx_,
                                                     capture.modifier,
                                                     *elem_type,
                                                     iterable_type.is_constant(),
                                                     is_lvalue_shape(resolving_, iterable),
                                                     "array or slice",
                                                     resolving_.ast.location_of(capture.payload))};
            if (!cap_result) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_, id, std::move(cap_result).error()));
            }
            resolving_.set_sema_type(capture.payload, **cap_result);

            if (capture.payload.is<ast::identifier_expr>()) {
                resolve_symbol_info(capture.payload, symbol_kind::VALUE);
            }
        }
        const auto& block{resolving_.ast.get_as<ast::block_stmt>(for_expr.block)};
        for (const auto& stmt : block) { TRY_RESOLVE(stmt); }
    }

    if (for_expr.non_break) { TRY_RESOLVE(*for_expr.non_break); }
    resolving_.set_sema_type(
        id, loop_type.is_poison() ? ctx_.get_builtin_resolved_type(type_kind::VOID_) : loop_type);
    last_type_.emplace(resolving_.get_sema_type(id));
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

    if (fn.is_type_expr) {
        const auto true_param_count{fn.parameters.size() + (fn.self ? 1UZ : 0UZ)};
        auto       fn_param_types{ctx_.pool.get_many_unsafe(true_param_count)};
        usize      p_idx{0};
        if (fn.self) {
            fn_param_types[p_idx++] = &ctx_.get_builtin_resolved_type(type_kind::OPAQUE);
        }
        for (const auto& param : fn.parameters) {
            TRY_RESOLVE(param.explicit_type);
            fn_param_types[p_idx++] = &denoted_type(*last_type_.take());
        }
        TRY_RESOLVE(fn.explicit_return_type);
        auto& return_type{denoted_type(*last_type_.take())};

        types::key_t fn_key{type_kind::FUNCTION, types::mut::CONSTANT};
        for (const auto* p : fn_param_types) { fn_key.imprint(*p); }
        fn_key.imprint(return_type);
        auto& fn_type{*ctx_.pool[fn_key]};
        fn_type.resolve_if<types::function>(
            fn_param_types, return_type, fn.self.has_value(), fn.variadic);

        auto& meta{*ctx_.pool[{type_kind::TYPE, types::mut::CONSTANT, fn_type}]};
        meta.resolve_if<types::meta_type>(fn_type);
        resolving_.set_sema_type(id, meta);
        return last_type_.emplace(meta);
    }

    // The entire function lives inside of its preallocated scope
    auto&       fn_type{resolving_.get_sema_type(id)};
    const scope s{table_stack_, fn_type.get_symbol_table_idx(), table_idx_};

    const function_boundary_guard fn_boundary{function_boundaries_, table_stack_.size() - 1};
    const open_function_guard     fn_node{open_function_nodes_, id};
    const self_recursion_guard    fn_self_ref{self_recursive_flags_, false};

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
        const auto& ident{resolving_.ast.get_as<ast::identifier_expr>(param.name)};
        if (auto sym{ctx_.registry.get_from_opt(table_idx_, ident.name)}) {
            sym->set_status(symbol_status::RESOLVING);
        }
        TRY_RESOLVE(param.explicit_type);

        auto& param_type{denoted_type(*last_type_.take())};
        // A `type`-typed value is always compile-time known, so `constexpr` adds nothing.
        if (param.is_constexpr && param_type.get_kind() == type_kind::TYPE) {
            ctx_.diags.emplace_back(
                "'constexpr' is redundant on a parameter of type 'type'; type values are "
                "always compile-time known",
                error::REDUNDANT_CONSTEXPR,
                resolving_.ast.location_of(param.name));
        }
        param_types[param_idx++] = &param_type;
        resolving_.set_sema_type(param.name, param_type);
        resolve_symbol_info(param.name, symbol_kind::VALUE);
    }

    TRY_RESOLVE(fn.explicit_return_type);
    auto& return_type{*last_type_.take()};
    ASSERT(!fn_type.is_resolved(), "Valued function must not be resolved");

    // A `constexpr` parameter makes the function a template, monomorphized per value
    if (any_param_generic(param_types) || any_param_constexpr(fn)) {
        fn_type.resolve<types::function>(
            param_types, return_type, fn.self.has_value(), fn.variadic);
        ctx_.generic_functions.register_function(
            fn_type, resolving_, id, fn, stdx::none, user_type_stack_.peek());
        register_impl_param_bounds(fn_type, fn);
        return last_type_.emplace(fn_type);
    }

    const auto is_auto_return{return_type.get_kind() == type_kind::AUTO};

    // A known return type lets a recursive call inside the body resolve against this signature
    if (!is_auto_return) {
        fn_type.resolve<types::function>(
            param_types, return_type, fn.self.has_value(), fn.variadic);
    }

    return_trackers_.emplace_back(return_tracker{
        .return_types   = {},
        .is_auto_return = is_auto_return,
        .expected_type  = is_auto_return ? stdx::none : stdx::option<type&>{return_type},
    });

    const auto& block{resolving_.ast.get_as<ast::block_stmt>(fn.body)};
    for (const auto& stmt : block) { TRY_RESOLVE(stmt); }

    auto tracker{std::move(return_trackers_.back())};
    return_trackers_.pop_back();

    auto& deduced_return_type{is_auto_return ? tracker.deduced_return_type(ctx_) : return_type};
    if (is_auto_return) {
        fn_type.resolve<types::function>(
            param_types, deduced_return_type, fn.self.has_value(), fn.variadic);
        resolving_.set_sema_type(fn.explicit_return_type, deduced_return_type);
    }

    const auto has_captures{!resolving_.get_captures(id).empty()};
    if (self_recursive_flags_.back() && has_captures) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "A closure cannot call itself by name; use @fnCtx() instead",
                             error::ILLEGAL_RECURSIVE_CLOSURE,
                             resolving_.ast.location_of(id)));
    }

    if (has_captures) { return last_type_.emplace(attach_closure_type(id, fn_type, fn.is_move)); }
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

        // A recursive call only needs the signature, which is resolved before the body is
        if (decl->value->is<ast::function_expr>()) {
            const auto fn_ty{target_mod.get_sema_type_opt(*decl->value)};
            return (fn_ty && fn_ty->is_resolved()) ? fn_ty : stdx::none;
        }

        const auto is_agg{decl->value->is<ast::struct_expr>() ||
                          decl->value->is<ast::union_expr>() || decl->value->is<ast::enum_expr>()};
        if ((!mod || (!mod->is_ptr() && !mod->is_ref())) && !is_agg) { return stdx::none; }
        return target_mod.get_sema_type_opt(*decl->value);
    }

    if (const auto alias{target_mod.ast.get_as_opt<ast::using_stmt>(*node)}) {
        return target_mod.get_sema_type_opt(alias->explicit_type);
    }
    return stdx::none;
}

} // namespace

auto type_resolver::get_resolved_symbol_type(symbol::data_t& symbol_data) -> type& {
    return symbol_data.visit(
        [](symbols::builtin& builtin) -> type& { return builtin.get_type(); },
        [this](symbols::label& label) -> type& {
            const auto defn{label.get_definition()};
            ASSERT(resolving_.has_sema_type(defn), "Resolved node has no type");
            return resolving_.get_sema_type(defn);
        },
        [this](auto& sym) -> type& {
            ASSERT(resolving_.has_sema_type(sym), "Directly indexable symbol was never typed");
            return resolving_.get_sema_type(sym);
        },
        [this](HasBothNameAndType auto& sym) -> type& {
            ASSERT(resolving_.has_sema_type(sym.name), "Symbol was never typed");
            auto& type{resolving_.get_sema_type(sym.name)};
            if (const auto explicit_opt{resolving_.get_sema_type_opt(sym.explicit_type)}) {
                const auto exp_kind{explicit_opt->get_kind()};
                ASSERT(type == *explicit_opt || exp_kind == type_kind::AUTO ||
                           exp_kind == type_kind::TYPE || is_generic_type(*explicit_opt) ||
                           is_assignable(type, *explicit_opt) || is_assignable(*explicit_opt, type),
                       "Symbol was resolved with mismatched type");
            }
            return type;
        },
        [this](HasNameOnly auto& sym) -> type& {
            ASSERT(resolving_.has_sema_type(sym.name), "Name-only sym was never typed");
            return resolving_.get_sema_type(sym.name);
        },
        [this](symbols::for_loop_capture& capture) -> type& {
            ASSERT(capture.payload.is<ast::identifier_expr>(), "Capture payload must be an ident");
            ASSERT(resolving_.has_sema_type(capture.payload), "For loop capture was never typed");
            return resolving_.get_sema_type(capture.payload);
        });
}

auto type_resolver::infer_capture_mode(capture_usage usage,
                                       const type&   captured_type,
                                       bool          force_move) noexcept -> types::capture_mode {
    // `move fn` copies every capture into the environment, even a mutated one
    if (force_move) { return types::capture_mode::VALUE; }
    if (usage == capture_usage::MUTATED) { return types::capture_mode::MUT_REF; }

    const auto kind{captured_type.get_kind()};
    const auto by_value{is_numeric(kind) || kind == type_kind::BOOL || kind == type_kind::POINTER};
    return by_value ? types::capture_mode::VALUE : types::capture_mode::REF;
}

auto type_resolver::attach_closure_type(ast::node_id fn_id, type& fn_type, bool is_move) -> type& {
    const auto captures{resolving_.get_captures(fn_id)};
    ASSERT(!captures.empty(), "attach_closure_type called with no captures");

    const auto idx{fn_type.get_symbol_table_idx()};
    auto*      closure_type{ctx_.pool[{type_kind::CLOSURE, types::mut::CONSTANT, idx}].get()};
    closure_type->set_symbol_table_idx(idx);

    auto  capture_span{ctx_.arena.make_span<types::closure_capture>(captures.size())};
    usize i{0};
    for (const auto& capture : captures) {
        auto lookup{ctx_.registry.lookup(table_stack_, capture.name)};
        ASSERT(lookup, "Recorded capture name failed to re-resolve in its enclosing scope");
        auto&      captured_type{get_resolved_symbol_type(lookup->get_data())};
        const auto mode{infer_capture_mode(capture.usage, captured_type, is_move)};

        // What the environment actually stores: the value itself, or a reference to it
        auto* storage_type{&captured_type};
        if (mode == types::capture_mode::REF) {
            storage_type = &ctx_.get_reference(types::mut::CONSTANT, captured_type);
        } else if (mode == types::capture_mode::MUT_REF) {
            storage_type = &ctx_.get_reference(types::mut::MUTABLE, captured_type);
        }

        capture_span[i++] =
            types::closure_capture{capture.name, &captured_type, storage_type, mode};
    }

    // Inject mutable self reference into closure call arguments
    const auto& public_sig{fn_type.get_data().as<types::function>()};
    auto        impl_params{ctx_.pool.get_many_unsafe(public_sig.params.size() + 1)};
    impl_params[0] = &ctx_.get_reference(types::mut::MUTABLE, *closure_type);
    for (usize p{0}; p < public_sig.params.size(); ++p) {
        impl_params[p + 1] = public_sig.params[p];
    }

    auto* impl_sig{ctx_.pool[{type_kind::FUNCTION, types::mut::CONSTANT, idx, true}].get()};
    impl_sig->resolve<types::function>(
        impl_params, public_sig.return_type, true, public_sig.is_variadic);

    closure_type->resolve<types::closure_t>(capture_span, fn_type, *impl_sig);
    resolving_.set_sema_type(fn_id, *closure_type);
    return *closure_type;
}

template <ast::IndexableID ID> auto type_resolver::resolve_symbol(ID id, symbol& sym) -> void {
    auto& symbol_data{sym.get_data()};
    switch (sym.get_status()) {
    case symbol_status::RESOLVED:
        // Identifier handles are not unique in the tree, but their symbol can be used to find root
        resolving_.set_sema_type(id, get_resolved_symbol_type(symbol_data));
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

        // Fields/params/enum members can't resolve out-of-order, so early use is an ordering
        // issue, not a self-reference cycle.
        const auto node{symbol_data.as_opt<symbols::node_t>()};
        if (!node) {
            ctx_.poison_symbol(sym);
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("'{}' is referenced before its declaration; forward references to "
                            "struct/union fields, function parameters, and enum members are not "
                            "supported; declare '{}' earlier",
                            sym.get_name(),
                            sym.get_name()),
                error::ILLEGAL_FIELD_ORDER_DEPENDENCY,
                resolving_.ast.location_of(id)));
        }
        resolve(*node);
        // Take the symbol's real type so forward references and mutual recursion resolve.
        if (sym.get_status() == symbol_status::RESOLVED &&
            sym.get_kind() != symbol_kind::POISONED) {
            resolving_.set_sema_type(id, get_resolved_symbol_type(symbol_data));
        } else {
            resolving_.set_sema_type(id, *last_type_.take());
        }
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

auto type_resolver::record_symbol_owner(ast::node_id       ref_id,
                                        usize              owner_table_idx,
                                        const mod::module& target_mod,
                                        const symbol&      sym) -> void {
    const auto node{sym.get_data().as_opt<symbols::node_t>()};
    if (!node) { return; }
    const auto decl{target_mod.ast.get_as_opt<ast::decl_stmt>(*node)};
    if (!decl || !decl->value) { return; }

    // Only a direct reference to a named function decays to a bare GIR symbol name at a call
    // or value site
    if (!target_mod.ast.get_as_opt<ast::function_expr>(*decl->value).has_value()) { return; }
    resolving_.set_resolved_symbol_owner(ref_id, owner_table_idx);
}

auto type_resolver::record_member_owner(ast::node_id           ref_id,
                                        type&                  object_type,
                                        ast::identifier_handle member) -> void {
    type* target{&object_type};
    if (const auto meta{target->get_data().as_opt<types::meta_type>()}) {
        target = &meta->instance;
    }
    if (const auto ptr{target->get_data().as_opt<types::pointer>()}) { target = &ptr->underlying; }
    if (const auto ref{target->get_data().as_opt<types::reference>()}) {
        target = &ref->underlying;
    }
    if (const auto fn{target->get_data().as_opt<types::function>()}) { target = &fn->return_type; }

    const auto enclosing{target->get_data().visit(
        [](const types::struct_t& s) -> stdx::option<const mod::module&> { return s.enclosing; },
        [](const types::union_t& u) -> stdx::option<const mod::module&> { return u.enclosing; },
        [](const types::enum_t& e) -> stdx::option<const mod::module&> { return e.enclosing; },
        [](const auto&) -> stdx::option<const mod::module&> { return stdx::none; })};
    if (!enclosing || !target->has_symbol_table_idx()) { return; }

    const auto  table_idx{target->get_symbol_table_idx()};
    const auto& member_ident{resolving_.ast.get_as<ast::identifier_expr>(member)};
    if (const auto sym{ctx_.registry.get_from_opt(table_idx, member_ident.name)}) {
        record_symbol_owner(ref_id, table_idx, *enclosing, *sym);
    }
}

auto type_resolver::record_type_ctor_member_call(ast::node_id call_id, const ast::call_expr& call)
    -> void {
    stdx::option<std::string_view> member_name;
    stdx::option<type&>            obj_type;
    if (const auto dot{resolving_.ast.get_as_opt<ast::dot_expr>(call.function)}) {
        member_name.emplace(resolving_.ast.get_as<ast::identifier_expr>(dot->member).name);
        obj_type = resolving_.get_sema_type_opt(dot->object);
    } else if (const auto imp{
                   resolving_.ast.get_as_opt<ast::implicit_access_expr>(call.function)}) {
        member_name.emplace(resolving_.ast.get_as<ast::identifier_expr>(imp->member).name);
        obj_type = implicit_type_stack_.peek();
    }
    if (!member_name || !obj_type) { return; }

    type* t{obj_type.get()};
    if (const auto meta{t->get_data().as_opt<types::meta_type>()}) { t = &meta->instance; }

    if (const auto prefix{ctx_.generic_functions.get_type_ctor_member_prefix(*t)}) {
        resolving_.set_generic_call_target(call_id, fmt::format("{}.{}", *prefix, *member_name));
    }
}

auto type_resolver::register_non_generic_type_ctor_members(type&                 result,
                                                           const ast::call_expr& ctor_call)
    -> void {
    const auto k{result.get_kind()};
    if (k != type_kind::STRUCT && k != type_kind::UNION && k != type_kind::ENUM) { return; }
    if (ctx_.generic_functions.get_type_ctor_member_prefix(result)) { return; }

    // Resolve the constructor's declaration and its owning module from `Ctor()` or `mod::Ctor()`.
    mod::module*          owner_mod{nullptr};
    stdx::option<symbol&> ctor_sym;
    if (const auto fn_ident{resolving_.ast.get_as_opt<ast::identifier_expr>(ctor_call.function)}) {
        if (!resolving_.root_table_idx) { return; }
        owner_mod = &resolving_;
        ctor_sym  = ctx_.registry.get_from_opt(*resolving_.root_table_idx, fn_ident->name);
    } else if (const auto mac{
                   resolving_.ast.get_as_opt<ast::module_access_expr>(ctor_call.function)}) {
        const auto mod_type{resolving_.get_sema_type_opt(mac->outer)};
        const auto m_data{mod_type ? mod_type->get_data().as_opt<types::module>() : stdx::none};
        if (!m_data || !m_data->imported.root_table_idx) { return; }
        owner_mod = &m_data->imported;
        const auto& inner{resolving_.ast.get_as<ast::identifier_expr>(mac->inner)};
        ctor_sym = ctx_.registry.get_from_opt(*owner_mod->root_table_idx, inner.name);
    }
    if (!owner_mod || !ctor_sym) { return; }

    const auto node{ctor_sym->get_data().as_opt<symbols::node_t>()};
    if (!node) { return; }
    const auto decl{owner_mod->ast.get_as_opt<ast::decl_stmt>(*node)};
    if (!decl || !decl->value) { return; }
    const auto fn_expr{owner_mod->ast.get_as_opt<ast::function_expr>(*decl->value)};
    if (!fn_expr) { return; }

    const auto& block{owner_mod->ast.get_as<ast::block_stmt>(fn_expr->body)};
    const auto  agg_node{returned_aggregate_node(owner_mod->ast, block)};
    if (!agg_node) { return; }

    const auto prefix{fmt::format(
        "tc{}", result.has_symbol_table_idx() ? result.get_symbol_table_idx() : usize{0})};
    register_type_ctor_members(ctx_, *owner_mod, *agg_node, result, result, prefix, {});
}

template <ast::IndexableID ID>
auto type_resolver::resolve_ident(ID id, const ast::identifier_expr& ident) -> void {
    const auto name{ident.name};
    auto       lookup{ctx_.registry.lookup_with_depth(table_stack_, name)};

    // Check for an undeclared identifier and poison the ident
    if (!lookup) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("Use of undeclared identifier '{}'", name),
                             error::UNDECLARED_IDENTIFIER,
                             resolving_.ast.location_of(id)));
    }

    // Record where this reference resolves to, for LSP go-to-definition
    resolving_.set_identifier_definition(
        id, {resolving_.path, lookup->symbol.get_symbol_span(resolving_)});
    if constexpr (std::same_as<ID, ast::node_id>) { resolving_.add_identifier_position(id); }

    // Belongs to an enclosing function's stack frame rather than the module/prelude scope
    if (!function_boundaries_.empty() && lookup->depth < function_boundaries_.back() &&
        lookup->depth >= function_boundaries_.front()) {
        // A function referring to its own name is recursion, not a captured free variable
        bool is_self_reference{false};
        if (const auto node{lookup->symbol.get_data().as_opt<symbols::node_t>()}) {
            if (const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*node)};
                decl && decl->value) {
                const ast::node_id value_id{*decl->value};
                const auto&        self_id{open_function_nodes_.back()};
                is_self_reference = value_id.get_kind() == self_id.get_kind() &&
                                    value_id.get_index() == self_id.get_index();
            }
        }

        if (is_self_reference) {
            self_recursive_flags_.back() = true;
        } else {
            const auto usage{in_mutating_context_ ? capture_usage::MUTATED : capture_usage::READ};

            // Find the innermost open function whose own scope actually contains the declaration
            usize owner_idx{0};
            for (usize i{function_boundaries_.size()}; i-- > 0;) {
                if (lookup->depth >= function_boundaries_[i]) {
                    owner_idx = i;
                    break;
                }
            }
            for (usize i{owner_idx + 1}; i < open_function_nodes_.size(); ++i) {
                resolving_.add_capture(open_function_nodes_[i], name, usage);
            }
        }
    }

    if constexpr (std::same_as<ID, ast::node_id>) {
        if (const auto located{ctx_.registry.lookup_with_table(table_stack_, name)}) {
            record_symbol_owner(id, located->table_idx, resolving_, located->symbol);
        }
    }

    resolve_symbol(id, lookup->symbol);
}

VISITOR_TEMPLATE_INIT(type_resolver, resolve_ident, const ast::identifier_expr&)

auto type_resolver::visit(ast::node_id id, const ast::identifier_expr& ident) -> void {
    PROFILE_FUNCTION();
    resolve_ident(id, ident);
}

auto type_resolver::visit(ast::node_id id, const ast::if_expr& if_expr) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(if_expr.condition);

    // Opportunistic fold of an `if constexpr` condition to try to not resolve dead code
    if (if_expr.constexpr_condition) {
        gir::const_eval evaluator{ctx_, resolving_};
        if (const auto cond_cv{evaluator.try_eval(if_expr.condition)}) {
            if (const auto folded{cond_cv->as_opt<bool>()}) {
                const auto arm_value_type{[&](ast::stmt_handle arm) -> type& {
                    if (const auto es{resolving_.ast.get_as_opt<ast::expr_stmt>(arm)}) {
                        if (const auto t{resolving_.get_sema_type_opt(es->expression)}) {
                            return *t;
                        }
                    }
                    return *last_type_;
                }};

                stdx::option<type&> live_type;
                if (*folded) {
                    TRY_RESOLVE(if_expr.consequence);
                    live_type.emplace(arm_value_type(if_expr.consequence));
                } else if (if_expr.alternate) {
                    TRY_RESOLVE(*if_expr.alternate);
                    live_type.emplace(arm_value_type(*if_expr.alternate));
                }
                last_type_.reset();
                auto& node_type{live_type ? *live_type
                                          : ctx_.get_builtin_resolved_type(type_kind::VOID_)};
                // Per-instantiation verdicts are captured and replayed by instantiate_generic.
                resolving_.if_constexpr_results.insert_or_assign(
                    id.get_index(),
                    *folded ? mod::if_branch::CONSEQUENCE : mod::if_branch::ALTERNATE);
                resolving_.set_sema_type(id, node_type);
                last_type_.emplace(node_type);
                return;
            }
        }
    }

    TRY_RESOLVE(if_expr.consequence);

    auto* branch_type{last_type_.take()};
    if (if_expr.alternate) {
        TRY_RESOLVE(*if_expr.alternate);
        if (const auto cons_expr{resolving_.ast.get_as_opt<ast::expr_stmt>(if_expr.consequence)}) {
            if (const auto alt_expr{
                    resolving_.ast.get_as_opt<ast::expr_stmt>(*if_expr.alternate)}) {
                if (const auto cons_type{resolving_.get_sema_type_opt(cons_expr->expression)}) {
                    if (const auto alt_type{resolving_.get_sema_type_opt(alt_expr->expression)}) {
                        if (!cons_type->is_poison() && !alt_type->is_poison() &&
                            cons_type->get_kind() != type_kind::VOID_) {
                            branch_type = cons_type.get();
                        }
                    }
                }
            }
        }
    }
    resolving_.set_sema_type(id, *branch_type);
    last_type_.emplace(*branch_type);
}

auto type_resolver::visit(ast::node_id id, const ast::index_expr& index) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(index.array);
    auto& array_type{*last_type_.take()};
    auto* target_type{&array_type};
    if (const auto ref{target_type->get_data().as_opt<types::reference>()}) {
        target_type = &ref->underlying;
    }
    auto& array_data{target_type->get_data()};

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

    // An open-ended range index (`x[lo..]`, `x[..hi]`, `x[..]`) needs `operand.len`, which a bare
    // pointer does not have.
    if (const auto range{resolving_.ast.get_as_opt<ast::range_expr>(index.index)};
        range && (!range->lhs || !range->rhs) && array_data.is<types::pointer>()) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "An open-ended range needs a known length; index a pointer with an "
                             "explicit `lo..hi` range instead",
                             error::ILLEGAL_OPEN_RANGE,
                             resolving_.ast.location_of(index.index)));
    }

    {
        auto&                        usize_type{ctx_.get_builtin_resolved_type(type_kind::USIZE)};
        const structural_guard       g{implicit_type_stack_, usize_type};
        const mutating_context_guard subscript_g{in_subscript_index_, true};
        TRY_RESOLVE(index.index);
    }
    auto& access_type{*last_type_.take()};

    // There may be a slice accessor which results in a slice type and should mirror parent
    if (access_type.get_data().is<types::slice>()) {
        // The subslice is writable iff the source container's elements are
        const auto mutability{container_element_mutability(*target_type)};
        last_type_.emplace(ctx_.get_slice(mutability, false, single_item_type));
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

// The cfg pass has already evaluated this `@cfgValue`; adopt the verdict's type
auto type_resolver::visit(ast::node_id id, const ast::cfg_value_expr&) -> void {
    PROFILE_FUNCTION();
    const auto it{resolving_.cfg_value_results.find(id.get_index())};
    if (it == resolving_.cfg_value_results.end() || it->second.is_predicate) {
        auto& bool_type{ctx_.get_builtin_resolved_type(type_kind::BOOL)};
        resolving_.set_sema_type(id, bool_type);
        last_type_.emplace(bool_type);
        return;
    }
    TRY_RESOLVE(it->second.chosen);
    auto& chosen_type{*last_type_};
    resolving_.set_sema_type(id, chosen_type);
    last_type_.emplace(chosen_type);
}

// `@cfg` statements are spliced away by the cfg pass before type resolution runs.
auto type_resolver::visit(ast::node_id, const ast::cfg_stmt&) -> void {
    UNREACHABLE("cfg_stmt must be removed by the cfg pass before type resolution");
}

auto type_resolver::visit(ast::node_id id, const ast::assignment_expr& assign) -> void {
    PROFILE_FUNCTION();
    {
        const mutating_context_guard g{in_mutating_context_};
        TRY_RESOLVE(assign.lhs);
    }
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

    // Implicit access needs type context established elsewhere; nothing has for a binary LHS.
    if (resolving_.ast.get_as_opt<ast::implicit_access_expr>(binary.lhs)) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            "Implicit access cannot appear on the left side of a binary expression; it "
            "requires the other operand to establish type context first",
            error::TYPE_MISMATCH,
            resolving_.ast.location_of(binary.lhs)));
    }

    TRY_RESOLVE(binary.lhs);
    auto* lhs_type{last_type_.take()};
    {
        const structural_guard g{implicit_type_stack_, *lhs_type};
        TRY_RESOLVE(binary.rhs);
    }
    auto& rhs_type{*last_type_.take()};

    if (is_integer(rhs_type.get_kind())) {
        if (const auto i32_node{resolving_.ast.get_as_opt<ast::i32_expr>(binary.lhs)}) {
            if (i32_node->value >= 0) {
                resolving_.set_sema_type(binary.lhs, rhs_type);
                lhs_type = &rhs_type;
            }
        }
    }

    switch (id.get_token_type()) {
    case syntax::token_type_t::LT:
    case syntax::token_type_t::LT_EQ:
    case syntax::token_type_t::GT:
    case syntax::token_type_t::GT_EQ:
    case syntax::token_type_t::EQ:
    case syntax::token_type_t::NEQ:
    case syntax::token_type_t::BOOLEAN_AND:
    case syntax::token_type_t::BOOLEAN_OR:
        last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::BOOL));
        break;
    default: last_type_.emplace(lhs_type); break;
    }

    resolving_.set_sema_type(id, *last_type_);
}

// Looks `name` up among the methods attached to `target` by `impl` blocks. Returns:
//   - `none`               : no such extension method is visible
//   - `ok(fn type)`        : exactly one visible method
//   - `err(AMBIGUOUS_...)` : more than one visible method with this name
auto type_resolver::register_impl_param_bounds(type& fn_type, const ast::function_expr& fn)
    -> void {
    if (fn.impl_bounds.empty()) { return; }
    std::vector<std::pair<u32, std::vector<const type*>>> entries;
    for (const auto& b : fn.impl_bounds) {
        std::vector<const type*> ifaces;
        for (const auto tid : b.interfaces) {
            resolve(tid);
            auto& t{denoted_type(*last_type_.take())};
            if (t.get_kind() == type_kind::INTERFACE) {
                ifaces.emplace_back(&t);
            } else if (!t.is_poison()) {
                ctx_.diags.emplace_back(
                    fmt::format("an `impl` parameter bound must name an interface; found `{}`",
                                ctx_.type_display_name(t)),
                    error::TYPE_MISMATCH,
                    resolving_.ast.location_of(tid));
            }
        }

        // An intersection bound whose interfaces share an associated-item name is rejected
        if (ifaces.size() > 1) {
            ankerl::unordered_dense::map<std::string_view, usize> assoc_counts;
            for (const auto* iface : ifaces) {
                if (const auto it{iface->get_data().as_opt<types::interface_t>()}) {
                    for (const auto n : it->assoc_type_names) { ++assoc_counts[n]; }
                    for (const auto n : it->assoc_const_names) { ++assoc_counts[n]; }
                }
            }

            for (const auto& [aname, count] : assoc_counts) {
                if (count > 1) {
                    ctx_.diags.emplace_back(
                        fmt::format("interfaces in this `impl (...)` bound both declare an "
                                    "associated item `{}`; split the parameter or use a "
                                    "sub-interface",
                                    aname),
                        error::CONFLICTING_ASSOC,
                        resolving_.ast.location_of(b.interfaces.front()));
                }
            }
        }

        if (!ifaces.empty()) { entries.emplace_back(b.param_index, std::move(ifaces)); }
    }
    if (!entries.empty()) { impl_param_bounds_.emplace(&fn_type, std::move(entries)); }
}

auto type_resolver::resolve_impl_method_access(const type&      target,
                                               std::string_view name,
                                               source_location  loc)
    -> stdx::option<stdx::result<gsl::not_null<type*>, diagnostic>> {
    const auto                              candidates{ctx_.impls.methods_of(target)};
    std::vector<const impl_record::method*> visible;
    bool                                    hidden_by_seal{false};
    for (const auto& em : candidates) {
        if (em.method->name != name || !em.method->fn_type) { continue; }
        // A non-`pub` trait-impl method is sealed to the interface's declaring module.
        if (em.record->interface_type && !em.method->is_pub) {
            if (declaring_module_of(*em.record->interface_type) != &resolving_) {
                hidden_by_seal = true;
                continue;
            }
        }
        visible.emplace_back(em.method.get());
    }

    if (visible.size() == 1) {
        for (const auto& em : candidates) {
            if (em.method == visible.front()) {
                if (em.record->from_parameterized) {
                    // A parameterized-impl method is emitted under a per-instantiation symbol
                    pending_param_impl_target_.emplace(
                        fmt::format("{}.{}", em.record->gir_prefix, name));
                } else {
                    pending_impl_method_owner_.emplace(em.record->body_scope_idx);
                }
                break;
            }
        }
        return stdx::result<gsl::not_null<type*>, diagnostic>{
            gsl::not_null<type*>{const_cast<type*>(visible.front()->fn_type.get())}};
    }

    // An interface default method the target inherits (not overridden by its impl). The signature
    // is rebuilt with `self` bound to the concrete target so the call and the emitted body agree.
    if (visible.empty()) {
        for (const auto* rec : ctx_.impls.records()) {
            if (!rec->target_type || rec->target_type != &target || !rec->interface_type) {
                continue;
            }
            const auto iface{rec->interface_type->get_data().as_opt<types::interface_t>()};
            if (!iface) { continue; }
            for (usize i{iface->requirement_count}; i < iface->method_names.size(); ++i) {
                if (iface->method_names[i] != name) { continue; }
                if (rec->find_method(name).has_value()) { continue; } // overridden
                const auto& fn{
                    resolving_.ast.get_as<ast::function_expr>(*iface->method_decl(i).signature)};
                pending_impl_method_owner_.emplace(rec->body_scope_idx);
                return stdx::result<gsl::not_null<type*>, diagnostic>{gsl::not_null<type*>{
                    &resolve_required_method_type(fn, const_cast<type&>(target))}};
            }
        }
    }

    if (visible.size() > 1) {
        return stdx::result<gsl::not_null<type*>, diagnostic>{stdx::err{diagnostic{
            fmt::format("call to `{}` is ambiguous: it is provided by more than one `impl`", name),
            error::AMBIGUOUS_METHOD,
            loc}}};
    }

    if (hidden_by_seal) {
        return stdx::result<gsl::not_null<type*>, diagnostic>{stdx::err{diagnostic{
            fmt::format("`{}` is a sealed interface method and is not callable from this module",
                        name),
            error::SEALED_METHOD,
            loc}}};
    }
    return stdx::none;
}

auto type_resolver::resolve_structural_access(type&                          object_type,
                                              ast::identifier_handle         member,
                                              source_location                object_location,
                                              stdx::option<std::string_view> object_name)
    -> stdx::result<gsl::not_null<type*>, diagnostic> {
    auto* target_type{&object_type};
    if (const auto ptr_data{target_type->get_data().as_opt<types::pointer>()}) {
        target_type = &ptr_data->underlying;
    } else if (const auto ref_data{target_type->get_data().as_opt<types::reference>()}) {
        target_type = &ref_data->underlying;
    }

    // e.g. `const f: fn(): T = .init;` fall through to the return type for its members
    if (const auto fn_data{target_type->get_data().as_opt<types::function>()}) {
        target_type = &fn_data->return_type;
    }

    auto&      object_data{target_type->get_data()};
    const auto enum_type{object_data.as_opt<types::enum_t>()};
    const auto struct_type{object_data.as_opt<types::struct_t>()};

    // A method reached on an interface-typed receiver resolves against the interface's set.
    if (const auto iface{object_data.as_opt<types::interface_t>()}) {
        const auto& member_ident{resolving_.ast.get_as<ast::identifier_expr>(member)};
        for (usize i{0}; i < iface->method_names.size(); ++i) {
            if (iface->method_names[i] == member_ident.name) { return iface->method_sigs[i]; }
        }
        return make_sema_err(fmt::format("interface has no member named '{}'", member_ident.name),
                             error::UNDECLARED_IDENTIFIER,
                             resolving_.ast.location_of(member));
    }
    const auto union_type{object_data.as_opt<types::union_t>()};
    const auto slice_type{object_data.as_opt<types::slice>()};
    const auto array_type{object_data.as_opt<types::array>()};
    const auto closure_type{object_data.as_opt<types::closure_t>()};

    if (closure_type) {
        const auto& member_ident{resolving_.ast.get_as<ast::identifier_expr>(member)};
        if (member_ident.name == "thunk") { return &closure_type->impl_signature; }
        return make_sema_err(
            fmt::format("Type 'closure' has no field named '{}'", member_ident.name),
            error::UNDECLARED_IDENTIFIER,
            resolving_.ast.location_of(member));
    }

    if (slice_type || array_type) {
        const auto& member_ident{resolving_.ast.get_as<ast::identifier_expr>(member)};
        auto&       underlying{slice_type ? slice_type->underlying : array_type->underlying};
        if (member_ident.name == "ptr") {
            return &ctx_.get_pointer(target_type->get_key().get_mut(), underlying);
        }
        if (member_ident.name == "len") {
            return &ctx_.get_builtin_resolved_type(type_kind::USIZE);
        }
        const auto type_kind_name{slice_type ? "slice" : "array"};
        return make_sema_err(
            object_name
                .transform([&](std::string_view name) -> std::string {
                    return fmt::format(
                        "Type '{}' has no field named '{}'", name, member_ident.name);
                })
                .value_or(fmt::format(
                    "Type '{}' has no field named '{}'", type_kind_name, member_ident.name)),
            error::UNDECLARED_IDENTIFIER,
            resolving_.ast.location_of(member));
    }

    if (!enum_type && !struct_type && !union_type) {
        const auto& member_ident{resolving_.ast.get_as<ast::identifier_expr>(member)};
        if (auto ext{resolve_impl_method_access(
                *target_type, member_ident.name, resolving_.ast.location_of(member))}) {
            return std::move(*ext);
        }
        return make_sema_err(
            fmt::format(
                "Can only access inner objects inside of structs, unions, and enums; found '{}'",
                type_kind_display_name(target_type->get_kind())),
            error::TYPE_MISMATCH,
            object_location);
    }

    ASSERT(target_type->has_symbol_table_idx(), "Structural should have a resolved table index");
    const auto table_idx{target_type->get_symbol_table_idx()};
    auto&      table{ctx_.registry.get(table_idx)};

    const auto& member_ident{resolving_.ast.get_as<ast::identifier_expr>(member)};
    const scope s{table_stack_, table_idx, table_idx_};
    auto        symbol_proxy{table.get_proxy_opt(member_ident.name)};

    if (!symbol_proxy) {
        if (auto ext{resolve_impl_method_access(
                *target_type, member_ident.name, resolving_.ast.location_of(member))}) {
            return std::move(*ext);
        }
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

auto type_resolver::get_rightmost_name(ast::expr_handle handle) const noexcept
    -> stdx::option<std::string_view> {
    ast::node_id current{*handle};
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

        if (const auto deref{resolving_.ast.get_as_opt<ast::dereference_expr>(current)}) {
            current = deref->rhs;
            continue;
        }

        return stdx::none;
    }
}

template <ast::IndexableID ID>
auto type_resolver::resolve_dot(ID id, const ast::dot_expr& dot) -> void {
    resolve(dot.object);
    if (last_type_->is_poison()) { return resolving_.set_sema_type(id, *last_type_); }
    auto& object_type{*last_type_.take()};

    // Desugared to auto and needs to be deferred like the call handler
    if (is_generic_type(object_type)) {
        auto& placeholder{*ctx_.pool[{type_kind::AUTO, types::mut::CONSTANT}]};
        resolving_.set_sema_type(dot.member, placeholder);
        resolving_.set_sema_type(id, placeholder);
        return last_type_.emplace(placeholder);
    }

    pending_impl_method_owner_.reset();
    pending_param_impl_target_.reset();
    auto result{resolve_structural_access(object_type,
                                          dot.member,
                                          resolving_.ast.location_of(dot.object),
                                          get_rightmost_name(dot.object))};
    if (!result) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(result).error()));
    }

    // An `impl`-attached method: pin the call target so the emitter names it the same way
    // `emit_top_level_impl` (inherent/trait) or `instantiate_impls_for` (parameterized) does.
    if (pending_impl_method_owner_ || pending_param_impl_target_) {
        if (pending_impl_method_owner_) {
            if constexpr (std::same_as<ID, ast::node_id>) {
                resolving_.set_resolved_symbol_owner(id, *pending_impl_method_owner_);
            }

            // `pending_param_impl_target_` is left set for `resolve_call`
            pending_impl_method_owner_.reset();
        }
        auto& member_type{**result};
        resolving_.set_sema_type(dot.member, member_type);
        resolving_.set_sema_type(id, member_type);
        resolving_.add_identifier_position(dot.member);
        return last_type_.emplace(member_type);
    }

    const auto unwrap_ref = [](type& t) -> type& {
        if (const auto p{t.get_data().as_opt<types::pointer>()}) { return p->underlying; }
        if (const auto r{t.get_data().as_opt<types::reference>()}) { return r->underlying; }
        return t;
    };

    if (object_type.get_kind() == type_kind::SLICE ||
        unwrap_ref(object_type).get_kind() == type_kind::INTERFACE) {
        auto& member_type{**result};
        resolving_.set_sema_type(dot.member, member_type);
        resolving_.set_sema_type(id, member_type);
        resolving_.add_identifier_position(dot.member);
        return last_type_.emplace(member_type);
    }

    const auto check_access = [&](const mod::module& enclosing,
                                  std::string_view   type_name,
                                  usize              field_count,
                                  auto&&             is_field_pub) -> bool {
        const auto& table{ctx_.registry.get(object_type.get_symbol_table_idx())};
        const auto& member_ident{resolving_.ast.get_as<ast::identifier_expr>(dot.member)};
        const auto  proxy{table.get_proxy_opt(member_ident.name)};

        // Record where this field/member access resolves to, for LSP go-to-definition
        if (proxy) {
            resolving_.set_identifier_definition(
                dot.member, {enclosing.path, proxy->symbol.get_symbol_span(enclosing)});
            resolving_.add_identifier_position(dot.member);
        }

        if (&enclosing == &resolving_) { return true; }
        if (!proxy) { return true; }

        const auto& [member_symbol, member_idx]{*proxy};
        const bool is_field{member_idx < field_count};
        const bool is_pub{is_field ? is_field_pub(member_idx) : member_symbol.is_public(enclosing)};

        if (!is_pub) {
            last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("{} '{}' of {} '{}' is private",
                            is_field ? "Field" : "Member",
                            member_ident.name,
                            type_name,
                            get_rightmost_name(dot.object).value_or("<expression>")),
                error::ILLEGAL_PRIVATE_ACCESS,
                resolving_.ast.location_of(dot.member)));
            return false;
        }
        return true;
    };

    const auto access_ok{object_type.get_data().visit(
        [&](const types::struct_t& s) {
            return check_access(s.enclosing, "struct", s.ast_fields.size(), [&](usize idx) {
                return s.ast_fields[idx].is_public();
            });
        },
        [&](const types::enum_t& e) {
            return check_access(
                e.enclosing, "enum", e.ast_enumerations.size(), [](usize) { return true; });
        },
        [&](const types::union_t& u) {
            return check_access(
                u.enclosing, "union", u.ast_fields.size(), [](usize) { return true; });
        },
        [](const auto&) { return true; })};
    if (!access_ok) { return; }

    // The structural resolver returns poisoned types in error conditions which can be bubbled here
    auto& member_type{*result.value()};
    if constexpr (std::same_as<ID, ast::node_id>) {
        if (!member_type.is_poison()) { record_member_owner(id, object_type, dot.member); }
    }
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
    auto& usize_type{ctx_.get_builtin_resolved_type(type_kind::USIZE)};

    // An omitted endpoint is filled from context: the indexed operand inside `[]`, or a sibling
    // iterable of a `for` loop. It is meaningless anywhere else.
    if ((!range.lhs || !range.rhs) && !in_subscript_index_ && !in_for_iterable_) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            "An open-ended range is only valid inside a subscript (`x[lo..]`, `x[..hi]`, `x[..]`) "
            "or a `for` loop paired with an array or slice",
            error::ILLEGAL_OPEN_RANGE,
            resolving_.ast.location_of(id)));
    }
    if (!range.rhs && id.get_token_type() == syntax::token_type_t::DOT_DOT_EQ) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "An inclusive range `..=` requires an upper bound",
                             error::ILLEGAL_OPEN_RANGE,
                             resolving_.ast.location_of(id)));
    }

    type* lhs_type{&usize_type};
    if (range.lhs) {
        TRY_RESOLVE(*range.lhs);
        lhs_type = last_type_.take();
    }
    if (range.rhs) {
        // Give a bare integer-literal upper bound the lower bound's type (`s.len .. 0`).
        const structural_guard g{implicit_type_stack_, *lhs_type};
        TRY_RESOLVE(*range.rhs);
        auto& rhs_type{*last_type_.take()};

        // ...and give a bare integer-literal lower bound the upper bound's type (`0 .. s.len`).
        if (range.lhs && is_integer(rhs_type.get_kind())) {
            if (const auto i32_node{resolving_.ast.get_as_opt<ast::i32_expr>(*range.lhs)};
                i32_node && i32_node->value >= 0) {
                resolving_.set_sema_type(*range.lhs, rhs_type);
                lhs_type = &rhs_type;
            }
        }
    }

    // Due to deferred type checking just use one endpoint's type for the placeholder slice.
    auto& slice_type{ctx_.get_slice(types::mut::CONSTANT, false, *lhs_type)};
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
    const auto struct_data_opt{struct_type.get_data().as_opt<types::struct_t>()};
    if (!struct_data_opt) {
        return diagnostic{"Initializer target is not a struct",
                          error::TYPE_MISMATCH,
                          resolving_.ast.location_of(init_id)};
    }
    const auto& struct_data{*struct_data_opt};

    // Check for duplicates
    for (const auto& [accessor_opt, value] : init.initializers) {
        if (!accessor_opt) {
            return diagnostic{"Struct initializers require '.field = ...' entries",
                              error::TYPE_MISMATCH,
                              resolving_.ast.location_of(value)};
        }
        const auto  accessor_node{resolving_.ast.get_as<ast::implicit_access_expr>(*accessor_opt)};
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
    for (const auto& [ident, _1, default_value, _2] : struct_data.ast_fields) {
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

    if (object_type_opt->get_data().is<types::deferred_call>()) {
        gir::const_eval evaluator{ctx_, resolving_};
        object_type_opt.emplace(evaluator.force_deferred_call(*object_type_opt));
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

    // `RowAlias{ a, b, c }` / `.{ a, b }` in an array-typed context
    type* array_object{&object_type};
    if (object_data.is<types::deferred_array>()) {
        gir::const_eval evaluator{ctx_, resolving_};
        array_object = &evaluator.force_deferred_array(object_type);
    }
    if (const auto arr_data{array_object->get_data().as_opt<types::array>()}) {
        for (const auto& entry : init.initializers) {
            if (entry.member) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    "An array initializer takes positional values, not '.field = ...' entries",
                    error::TYPE_MISMATCH,
                    resolving_.ast.location_of(*entry.member)));
            }
        }
        if (num_initializers != arr_data->len) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 fmt::format("Array of length {} initialized with {} value(s)",
                                             arr_data->len,
                                             num_initializers),
                                 error::ARITY_MISMATCH,
                                 resolving_.ast.location_of(id)));
        }
        for (const auto& entry : init.initializers) {
            const structural_guard g{implicit_type_stack_, arr_data->underlying};
            TRY_RESOLVE(entry.value);
        }
        resolving_.set_sema_type(id, *array_object);
        return last_type_.emplace(*array_object);
    }

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

    for (const auto& [accessor_opt, value] : init.initializers) {
        if (!accessor_opt) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 "Struct and union initializers require '.field = ...' entries",
                                 error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(value)));
        }
        const auto accessor{*accessor_opt};

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
        label_data.add_yield_type(ctx_.get_builtin_resolved_type(type_kind::VOID_));
    }

    // The last type inherits the result type to help propagation of poison
    auto& result_type{*label_data.get_yield_types()[0]};
    ASSERT(result_type.is_resolved(), "The label's inner type should've been resolved");
    label_type.resolve_if<type::data_t>(result_type.get_data());
    resolving_.set_sema_type(label.name, result_type);
    resolving_.set_sema_type(id, result_type);
    last_type_.emplace(result_type);
}

namespace {

// Collects potential duplicate implicit access match arms for the structural type
auto gather_arm_duplicates(gsl::span<const ast::match_expr::arm> arms,
                           mod::module&                          resolving,
                           type_resolver::structural_validator&  validator,
                           bool require_implicit_access) -> stdx::option<diagnostic> {
    for (const auto& arm : arms) {
        for (const auto& pattern : arm.patterns) {
            if (pattern.is<ast::discarded>()) { continue; }

            // It's only possible to verify access expressions
            const auto pattern_node{resolving.ast.get_as_opt<ast::implicit_access_expr>(pattern)};
            if (!pattern_node) {
                if (require_implicit_access) {
                    return diagnostic{
                        "Match arm may only have an implicit access pattern in this context",
                        error::ILLEGAL_MATCH_PATTERN,
                        resolving.ast.location_of(pattern)};
                }
                continue;
            }

            const auto& ident{resolving.ast.get_as<ast::identifier_expr>(pattern_node->member)};
            if (!validator.seen.insert(ident.name).second) {
                validator.duplicates.emplace_back(ident.name);
            }
            validator.provided.emplace_back(ident.name);
        }
    }
    return stdx::none;
}

} // namespace

auto type_resolver::validate_enum_arms(ast::node_id           match_id,
                                       const ast::match_expr& match,
                                       type& enum_type) -> stdx::option<diagnostic> {
    enum_validator_.clear();

    if (const auto e_data{enum_type.get_data().as_opt<types::enum_t>()};
        e_data && e_data->non_exhaustive && !match.catch_all_idx) {
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
    const auto union_data_opt{union_type.get_data().as_opt<types::union_t>()};
    if (!union_data_opt) { return stdx::none; }
    const auto& union_data{*union_data_opt};

    // An untagged (extern) union has no runtime discriminant to check a field against
    if (union_data.is_untagged) {
        return diagnostic{"Cannot match on an untagged union; it has no runtime tag",
                          error::TYPE_MISMATCH,
                          resolving_.ast.location_of(match.matcher)};
    }

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
        for (const auto& [ident, _1, _2] : union_data.ast_fields) {
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

auto type_resolver::resolve_type_match(ast::node_id           id,
                                       const ast::match_expr& match,
                                       type&                  matcher_type) -> void {
    PROFILE_FUNCTION();

    if (!match.catch_all_idx) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "A 'match' on a type requires a catch-all '_' arm",
                             error::ILLEGAL_MATCH_PATTERN,
                             resolving_.ast.location_of(id)));
    }

    gir::const_eval evaluator{ctx_, resolving_};

    // Resolve the scrutinee to a concrete type when it is known in this context
    stdx::option<type&> concrete;
    if (auto& m{denoted_type(matcher_type)};
        m.is_resolved() && m.get_kind() != type_kind::TYPE && m.get_kind() != type_kind::AUTO) {
        concrete.emplace(m);
    } else if (const auto cv{evaluator.try_eval(match.matcher)};
               cv && cv->is<stdx::option<sema::type&>>()) {
        if (const stdx::option<sema::type&> t{cv->as<stdx::option<sema::type&>>()};
            t && t->is_resolved()) {
            concrete.emplace(denoted_type(*t));
        }
    }

    // Resolve/validate every arm pattern and pick the arm a concrete scrutinee selects.
    stdx::opt_size selected;
    for (usize i{0}; i < match.arms.size(); ++i) {
        const auto& arm{match.arms[i]};
        auto&       arm_table_type{resolving_.get_sema_type(arm)};
        const scope arm_scope{table_stack_, arm_table_type.get_symbol_table_idx(), table_idx_};

        if (arm.capture) {
            return last_type_.emplace(ctx_.poison_node(resolving_,
                                                       id,
                                                       "A 'match' on a type cannot bind a capture",
                                                       error::ILLEGAL_MATCH_PATTERN,
                                                       resolving_.ast.location_of(*arm.capture)));
        }
        if (i == *match.catch_all_idx) { continue; }

        for (const auto& pattern : arm.patterns) {
            resolve(pattern);
            if (last_type_->is_poison()) {
                return resolving_.set_sema_type(id, *last_type_.take());
            }
            auto& pat_type{*last_type_.take()};

            // A pattern that folds to a concrete value is not a type.
            if (const auto pat_cv{evaluator.try_eval(pattern)};
                pat_cv && !pat_cv->is_poison() && !pat_cv->is<stdx::option<sema::type&>>()) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     "A 'match' on a type expects every arm pattern to be a type",
                                     error::ILLEGAL_MATCH_PATTERN,
                                     resolving_.ast.location_of(pattern)));
            }

            if (concrete && !selected) {
                if (auto& pat_concrete{denoted_type(pat_type)};
                    pat_concrete.is_resolved() &&
                    sema::is_same_unqualified(*concrete, pat_concrete)) {
                    selected.emplace(i);
                }
            }
        }
    }

    if (concrete && !selected) { selected = match.catch_all_idx; }

    // Type-check only the live arm's body
    const usize live_arm{selected ? *selected : *match.catch_all_idx};
    const auto& live{match.arms[live_arm]};
    {
        auto&       live_table_type{resolving_.get_sema_type(live)};
        const scope live_scope{table_stack_, live_table_type.get_symbol_table_idx(), table_idx_};
        TRY_RESOLVE(live.dispatch);
    }

    type* result_type{&ctx_.get_builtin_resolved_type(type_kind::VOID_)};
    if (const auto es{resolving_.ast.get_as_opt<ast::expr_stmt>(live.dispatch)}) {
        if (const auto inner{resolving_.get_sema_type_opt(es->expression)}) {
            if (!inner->is_poison() && inner->get_kind() != type_kind::VOID_) {
                result_type = inner.get();
            }
        }
    }

    if (selected) { resolving_.match_arm_results.insert_or_assign(id.get_index(), *selected); }
    resolving_.set_sema_type(id, *result_type);
    last_type_.emplace(*result_type);
}

auto type_resolver::resolve_constexpr_match(ast::node_id           id,
                                            const ast::match_expr& match,
                                            type&                  matcher_type) -> void {
    PROFILE_FUNCTION();

    gir::const_eval evaluator{ctx_, resolving_};
    const auto      scrutinee{evaluator.try_eval(match.matcher)};
    if (!scrutinee || scrutinee->is_poison()) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "'match constexpr' requires a compile-time-known scrutinee",
                             error::CONSTEXPR_EVALUATION_FAILED,
                             resolving_.ast.location_of(match.matcher)));
    }

    stdx::opt_size selected;
    for (usize i{0}; i < match.arms.size() && !selected; ++i) {
        if (match.catch_all_idx && i == *match.catch_all_idx) { continue; }
        for (const auto& pattern : match.arms[i].patterns) {
            if (evaluator.arm_pattern_matches(pattern, *scrutinee)) {
                selected.emplace(i);
                break;
            }
        }
    }
    if (!selected) { selected = match.catch_all_idx; }
    if (!selected) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "'match constexpr' has no arm matching the scrutinee and no '_' arm",
                             error::CONSTEXPR_EVALUATION_FAILED,
                             resolving_.ast.location_of(id)));
    }

    // Only the live arm is type-checked; the rest is dead code.
    const auto& live{match.arms[*selected]};
    {
        auto&       live_table_type{resolving_.get_sema_type(live)};
        const scope live_scope{table_stack_, live_table_type.get_symbol_table_idx(), table_idx_};

        if (live.capture && live.capture->is<ast::identifier_expr>()) {
            if (scrutinee->is<stdx::option<sema::type&>>()) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     "'match constexpr' on a type value cannot bind a capture",
                                     error::ILLEGAL_MATCH_PATTERN,
                                     resolving_.ast.location_of(*live.capture)));
            }
            if (!live.modifier.is_value()) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    "'match constexpr' captures cannot use a reference or pointer modifier",
                    error::ILLEGAL_MATCH_PATTERN,
                    resolving_.ast.location_of(*live.capture)));
            }

            type* cap_type{&denoted_type(matcher_type)};
            if (const auto ud{matcher_type.get_data().as_opt<types::union_t>()}) {
                if (const auto ia{resolving_.ast.get_as_opt<ast::implicit_access_expr>(
                        *live.primary_pattern())}) {
                    const auto& table{ctx_.registry.get(matcher_type.get_symbol_table_idx())};
                    const auto& pident{resolving_.ast.get_as<ast::identifier_expr>(ia->member)};
                    cap_type = &ud->type_at(table.get_proxy(pident.name).index);
                }
            }
            resolving_.set_sema_type(*live.capture, *cap_type);
            resolve_symbol_info(*live.capture, symbol_kind::VALUE);
        }

        TRY_RESOLVE(live.dispatch);
    }

    type* result_type{&ctx_.get_builtin_resolved_type(type_kind::VOID_)};
    if (const auto es{resolving_.ast.get_as_opt<ast::expr_stmt>(live.dispatch)}) {
        if (const auto inner{resolving_.get_sema_type_opt(es->expression)}) {
            if (!inner->is_poison() && inner->get_kind() != type_kind::VOID_) {
                result_type = inner.get();
            }
        }
    }

    resolving_.match_arm_results.insert_or_assign(id.get_index(), *selected);
    resolving_.set_sema_type(id, *result_type);
    last_type_.emplace(*result_type);
}

auto type_resolver::visit(ast::node_id id, const ast::match_expr& match) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(match.matcher);
    auto& matcher_type{*last_type_.take()};

    // `match constexpr` folds its scrutinee and type-checks only the arm it selects.
    if (match.is_constexpr) { return resolve_constexpr_match(id, match, matcher_type); }

    // A scrutinee that denotes a compile-time `type` takes the type-match path: only the
    // selected arm is checked/emitted, `if constexpr`-style.
    {
        const auto denotes_type{[&](auto&& self, ast::node_id n) -> bool {
            const auto& node{resolving_.ast[n]};
            if (node.template is<ast::struct_expr>() || node.template is<ast::enum_expr>() ||
                node.template is<ast::union_expr>()) {
                return true;
            }
            if (const auto arr{resolving_.ast.get_as_opt<ast::array_expr>(n)}) {
                return arr->is_type_expr;
            }
            if (const auto fx{resolving_.ast.get_as_opt<ast::function_expr>(n)}) {
                return fx->is_type_expr;
            }
            if (const auto addr{resolving_.ast.get_as_opt<ast::address_of_expr>(n)}) {
                return self(self, *addr->rhs);
            }
            if (const auto ref{resolving_.ast.get_as_opt<ast::reference_expr>(n)}) {
                return self(self, *ref->rhs);
            }
            if (const auto id_e{resolving_.ast.get_as_opt<ast::identifier_expr>(n)}) {
                const auto sym{ctx_.registry.lookup(table_stack_, id_e->name)};
                if (!sym) { return false; }
                if (sym->has_kind() && sym->get_kind() == symbol_kind::TYPE) { return true; }
                if (const auto nd{sym->get_data().template as_opt<symbols::node_t>()}) {
                    if (const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*nd)};
                        decl && decl->value &&
                        (decl->has_modifier(ast::decl_modifiers::CONSTANT) ||
                         decl->has_modifier(ast::decl_modifiers::CONSTEXPR))) {
                        return self(self, **decl->value);
                    }
                }
            }
            return false;
        }};

        bool matcher_denotes_type{denotes_type(denotes_type, *match.matcher)};
        // A `T: type` parameter resolves to its concrete argument inside an instantiation,
        // so also treat a `match` whose first real arm pattern is a type as a type match.
        if (!matcher_denotes_type) {
            for (usize i{0}; i < match.arms.size(); ++i) {
                if (match.catch_all_idx && i == *match.catch_all_idx) { continue; }
                matcher_denotes_type = denotes_type(denotes_type, *match.arms[i].primary_pattern());
                break;
            }
        }
        if (!matcher_denotes_type) {
            gir::const_eval type_probe{ctx_, resolving_};
            const auto      cv{type_probe.try_eval(match.matcher)};
            matcher_denotes_type = cv && cv->is<stdx::option<sema::type&>>();
        }
        // `match` on an enum/union type keeps its variant-enumeration semantics; empty and
        // incomplete types keep their existing "cannot match on ..." diagnostics.
        const auto denoted_kind{denoted_type(matcher_type).get_kind()};
        if (denoted_kind == type_kind::ENUM || denoted_kind == type_kind::UNION ||
            (denoted_kind != type_kind::TYPE && !is_value_type(denoted_kind))) {
            matcher_denotes_type = false;
        }
        if (matcher_denotes_type) { return resolve_type_match(id, match, matcher_type); }
    }

    // The expression must resolve to a single type on pass 3
    stdx::option<type&> first_type;
    bool                matcher_is_const{false};
    if (const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(match.matcher)}) {
        if (const auto sym{ctx_.registry.lookup(table_stack_, ident->name)}) {
            if (const auto node{sym->get_data().as_opt<symbols::node_t>()}) {
                if (const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*node)}) {
                    matcher_is_const = decl->has_modifier(ast::decl_modifiers::CONSTANT) ||
                                       decl->has_modifier(ast::decl_modifiers::CONSTEXPR);
                }
            }
        }
    }
    const bool matcher_is_addressable{is_lvalue_shape(resolving_, match.matcher)};

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
        case type_kind::VOID_:
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
        case type_kind::NULLPTR:
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("Can only match on integers, bytes, and booleans; found '{}'",
                            type_kind_display_name(matcher_type.get_kind())),
                sema::error::TYPE_MISMATCH,
                resolving_.ast.location_of(match.matcher)));
        default: UNREACHABLE("Builtin types should never take this type kind");
        }

        // A range arm can never make an integer/byte match exhaustive on its own.
        const bool has_range_arm{std::ranges::any_of(match.arms, [&](const auto& arm) {
            return std::ranges::any_of(arm.patterns, [&](const auto& p) {
                return resolving_.ast.get_as_opt<ast::range_expr>(*p).has_value();
            });
        })};
        if (has_range_arm) {
            if (matcher_type.get_kind() == type_kind::BOOL) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     "Range patterns are not allowed when matching on 'bool'",
                                     sema::error::ILLEGAL_MATCH_PATTERN,
                                     resolving_.ast.location_of(match.matcher)));
            }
            if (!match.catch_all_idx) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     "A 'match' with a range pattern requires a catch-all '_' arm",
                                     sema::error::ILLEGAL_MATCH_PATTERN,
                                     resolving_.ast.location_of(match.matcher)));
            }
        }

        if (!match.catch_all_idx) {
            // With a required arm count the total pattern count must match
            usize listed_pattern_count{0};
            for (const auto& arm : match.arms) { listed_pattern_count += arm.patterns.size(); }
            if (required_arm_count && *required_arm_count != listed_pattern_count) {
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

    // Constant scalar values and range intervals, closed on both ends, for overlap detection.
    struct arm_interval {
        i64                       lo;
        i64                       hi;
        ast::match_pattern_handle pat;
    };
    std::vector<arm_interval> const_intervals;
    gir::const_eval           interval_probe{ctx_, resolving_};
    const bool                scalar_match{matcher_data.is<types::builtin_type>() &&
                            matcher_type.get_kind() != type_kind::BOOL};

    // Each arm was assigned a new scope index on the first pass
    for (const auto& arm : match.arms) {
        // Tabled types have prefilled types that should be pushed on the table stack
        auto&       arm_table_type{resolving_.get_sema_type(arm)};
        const scope scope{table_stack_, arm_table_type.get_symbol_table_idx(), table_idx_};

        if (arm.capture && arm.capture->is<ast::identifier_expr>()) {
            // Unions implicitly unpack the value since the field is guaranteed to be valid
            stdx::option<type&> base_type;
            if (const auto union_data{matcher_data.as_opt<types::union_t>()}) {
                const auto& table{ctx_.registry.get(matcher_type.get_symbol_table_idx())};
                // Every listed variant must carry the same payload type to share one capture.
                for (const auto& pat : arm.patterns) {
                    const auto ia{resolving_.ast.get_as_opt<ast::implicit_access_expr>(*pat)};
                    ASSERT(ia, "Union validator failed to error");
                    const auto& pident{resolving_.ast.get_as<ast::identifier_expr>(ia->member)};
                    auto&       pty{union_data->type_at(table.get_proxy(pident.name).index)};
                    if (!base_type) {
                        base_type.emplace(pty);
                    } else if (!sema::is_same_unqualified(*base_type, pty)) {
                        return last_type_.emplace(ctx_.poison_node(
                            resolving_,
                            id,
                            "A capture on a multi-variant match arm requires every listed "
                            "variant to carry the same payload type",
                            error::ILLEGAL_MATCH_PATTERN,
                            resolving_.ast.location_of(*arm.capture)));
                    }
                }
            } else {
                base_type.emplace(matcher_type);
            }

            auto cap_result{resolve_capture_modifier(ctx_,
                                                     arm.modifier,
                                                     *base_type,
                                                     matcher_is_const,
                                                     matcher_is_addressable,
                                                     "value",
                                                     resolving_.ast.location_of(*arm.capture))};
            if (!cap_result) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_, id, std::move(cap_result).error()));
            }
            resolving_.set_sema_type(*arm.capture, **cap_result);
            resolve_symbol_info(*arm.capture, symbol_kind::VALUE);
        }

        // Each pattern is resolved against the matcher's type
        for (const auto& pattern : arm.patterns) {
            if (pattern.is<ast::discarded>()) { continue; }

            const auto range{resolving_.ast.get_as_opt<ast::range_expr>(*pattern)};
            if (range && !scalar_match) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    "Range patterns are only valid when matching on an integer or byte value",
                    error::ILLEGAL_MATCH_PATTERN,
                    resolving_.ast.location_of(*pattern)));
            }

            if (range && (!range->lhs || !range->rhs)) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     "A range pattern needs both endpoints; open-ended ranges are "
                                     "only valid inside a subscript",
                                     error::ILLEGAL_MATCH_PATTERN,
                                     resolving_.ast.location_of(*pattern)));
            }

            {
                const structural_guard pattern_g{implicit_type_stack_, matcher_type};
                if (range) {
                    TRY_RESOLVE(*range->lhs);
                    TRY_RESOLVE(*range->rhs);
                } else {
                    TRY_RESOLVE(pattern);
                }
            }

            if (!scalar_match) { continue; }
            if (range) {
                const bool inclusive{(*pattern).get_token_type() ==
                                     syntax::token_type_t::DOT_DOT_EQ};
                const auto lo{interval_probe.try_eval(*range->lhs)};
                const auto hi{interval_probe.try_eval(*range->rhs)};
                if (!lo || !hi) { continue; }
                const auto lo_i{lo->as_int_opt()};
                const auto hi_i{hi->as_int_opt()};
                if (!lo_i || !hi_i) { continue; }
                const i64 hi_closed{inclusive ? *hi_i : *hi_i - 1};
                if (hi_closed < *lo_i) {
                    return last_type_.emplace(ctx_.poison_node(
                        resolving_,
                        id,
                        "Range pattern is empty; its lower bound exceeds its upper bound",
                        error::ILLEGAL_MATCH_PATTERN,
                        resolving_.ast.location_of(*pattern)));
                }
                const_intervals.emplace_back(*lo_i, hi_closed, pattern);
            } else if (const auto pv{interval_probe.try_eval(*pattern)}) {
                if (const auto v{pv->as_int_opt()}) {
                    const_intervals.emplace_back(*v, *v, pattern);
                }
            }
        }
        TRY_RESOLVE(arm.dispatch);

        // Only an expr_stmt arm can yield a value (blocks never do, per emit_stmt_as_value); a
        // block's own resolved type is just its scope handle, not a value type, so it's ignored.
        type* arm_dispatch_type{&ctx_.get_builtin_resolved_type(type_kind::VOID_)};
        if (const auto expr_stmt_node{resolving_.ast.get_as_opt<ast::expr_stmt>(arm.dispatch)}) {
            if (const auto inner_type{resolving_.get_sema_type_opt(expr_stmt_node->expression)}) {
                if (!inner_type->is_poison() && inner_type->get_kind() != type_kind::VOID_) {
                    arm_dispatch_type = inner_type.get();
                }
            }
        }

        if ((!first_type || first_type->get_kind() == type_kind::VOID_) &&
            arm_dispatch_type->get_kind() != type_kind::VOID_) {
            first_type = *arm_dispatch_type;
        }
    }

    // Two arm patterns that both fold to constants may not cover the same value.
    for (usize i{0}; i < const_intervals.size(); ++i) {
        for (usize j{i + 1}; j < const_intervals.size(); ++j) {
            const auto& a{const_intervals[i]};
            const auto& b{const_intervals[j]};
            if (a.lo <= b.hi && b.lo <= a.hi) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     "This match arm pattern overlaps an earlier arm",
                                     error::ILLEGAL_MATCH_PATTERN,
                                     resolving_.ast.location_of(b.pat)));
            }
        }
    }

    if (!first_type) { first_type.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_)); }
    resolving_.set_sema_type(id, *first_type);
    last_type_.emplace(*first_type);
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
    {
        const mutating_context_guard g{in_mutating_context_,
                                       ref_addr_of_is_mutable(id) == types::mut::MUTABLE};
        TRY_RESOLVE(ref.rhs);
    }
    auto& rhs_type{*last_type_.take()};

    // References already behave like values
    if (rhs_type.get_kind() == type_kind::REFERENCE) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "Cannot take a reference to an already-reference-typed value; pass it "
                             "directly to alias the same referent",
                             error::ILLEGAL_REFERENCE_TO_REFERENCE,
                             resolving_.ast.location_of(id)));
    }

    auto& new_type{ctx_.get_reference(ref_addr_of_is_mutable(id), rhs_type)};
    new_type.resolve<types::reference>(rhs_type);

    resolving_.set_sema_type(id, new_type);
    last_type_.emplace(new_type);
}

auto type_resolver::visit(ast::node_id id, const ast::address_of_expr& adr_of) -> void {
    PROFILE_FUNCTION();
    {
        const mutating_context_guard g{in_mutating_context_,
                                       ref_addr_of_is_mutable(id) == types::mut::MUTABLE};
        TRY_RESOLVE(adr_of.rhs);
    }
    auto& rhs_type{*last_type_.take()};

    gsl::not_null<type*> pointee{&rhs_type};
    if (const auto ref{rhs_type.get_data().as_opt<types::reference>()}) {
        pointee = &ref->underlying;
    }

    auto& new_type{ctx_.get_pointer(ref_addr_of_is_mutable(id), *pointee)};
    new_type.resolve_if<types::pointer>(*pointee);
    resolving_.set_sema_type(id, new_type);
    last_type_.emplace(new_type);
}

auto type_resolver::visit(ast::node_id id, const ast::dereference_expr& deref) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(deref.rhs);
    auto& rhs_type{*last_type_.take()};

    // Check for a pointer or reference and update to the underlying type to enforce dereference
    // semantics
    if (const auto pointer{rhs_type.get_data().as_opt<types::pointer>()}) {
        last_type_.emplace(pointer->underlying);
    } else if (const auto ref{rhs_type.get_data().as_opt<types::reference>()}) {
        last_type_.emplace(ref->underlying);
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
    if (id.get_token_type() == syntax::token_type_t::BANG) {
        last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::BOOL));
    }
    resolving_.set_sema_type(id, *last_type_);
}

namespace {

enum class unwrap_family {
    RESULT,
    OPTIONAL,
};

struct unwrap_shape {
    unwrap_family family;
    usize         payload_idx;
    usize         diverge_idx;
};

// Recognizes a tagged two-field union shaped like `union { ok: T, err: E }` (RESULT) or
// `union { some: T, none: void }` (OPTIONAL).
[[nodiscard]] auto classify_unwrap_union(const type& t) -> stdx::option<unwrap_shape> {
    const auto ud{t.get_data().as_opt<types::union_t>()};
    if (!ud || ud->is_untagged || ud->fields.size() != 2) { return stdx::none; }

    const auto& enclosing{ud->enclosing};
    const auto  name_of{[&](usize i) -> std::string_view {
        return enclosing.ast.get_as<ast::identifier_expr>(ud->ast_fields[i].name).name;
    }};
    const auto  n0{name_of(0)}, n1{name_of(1)};

    if (n0 == "ok" && n1 == "err") { return unwrap_shape{unwrap_family::RESULT, 0, 1}; }
    if (n0 == "err" && n1 == "ok") { return unwrap_shape{unwrap_family::RESULT, 1, 0}; }
    if (n0 == "some" && n1 == "none") { return unwrap_shape{unwrap_family::OPTIONAL, 0, 1}; }
    if (n0 == "none" && n1 == "some") { return unwrap_shape{unwrap_family::OPTIONAL, 1, 0}; }
    return stdx::none;
}

} // namespace

auto type_resolver::visit(ast::node_id id, const ast::unwrap_expr& unwrap) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(unwrap.operand);
    auto& operand_type{*last_type_.take()};

    const bool is_question{id.get_token_type() == syntax::token_type_t::QUESTION};
    const auto loc{resolving_.ast.location_of(id)};

    const auto shape{classify_unwrap_union(operand_type)};
    if (!shape) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            fmt::format("the postfix '{}' operator expects a tagged union shaped like "
                        "'union {{ ok: T, err: E }}' or 'union {{ some: T, none: void }}'; "
                        "its operand has type '{}'",
                        is_question ? "?" : "!",
                        operand_type.to_string()),
            error::UNWRAP_ON_NON_RESULT,
            loc));
    }

    const auto& operand_union{operand_type.get_data().as<types::union_t>()};
    auto&       payload_type{operand_union.type_at(shape->payload_idx)};

    // `expr!` just projects the success payload with lowering handling the discriminant check
    if (!is_question) {
        resolving_.set_sema_type(id, payload_type);
        return last_type_.emplace(payload_type);
    }

    if (open_function_nodes_.empty()) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "the '?' operator can only be used inside a function",
                             error::UNWRAP_OUTSIDE_FUNCTION,
                             loc));
    }

    auto&                     fn_type{resolving_.get_sema_type(open_function_nodes_.back())};
    stdx::option<const type&> ret_type;
    if (const auto fd{fn_type.get_data().as_opt<types::function>()}) {
        ret_type.emplace(fd->return_type);
    } else if (const auto cd{fn_type.get_data().as_opt<types::closure_t>()}) {
        if (const auto sd{cd->signature.get_data().as_opt<types::function>()}) {
            ret_type.emplace(sd->return_type);
        }
    }
    if (!ret_type) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            "the '?' operator requires the enclosing function's return type to be written "
            "explicitly rather than inferred with 'auto'",
            error::UNWRAP_RETURN_TYPE_MISMATCH,
            loc));
    }

    const auto ret_shape{classify_unwrap_union(*ret_type)};
    if (!ret_shape || ret_shape->family != shape->family) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            fmt::format(
                "the '?' operator propagates a '{}' but the enclosing function returns '{}', "
                "which is not a matching {}",
                operand_type.to_string(),
                ret_type->to_string(),
                shape->family == unwrap_family::RESULT ? "'union { ok: _, err: E }'"
                                                       : "'union { some: _, none: void }'"),
            error::UNWRAP_RETURN_TYPE_MISMATCH,
            loc));
    }

    resolving_.set_sema_type(id, payload_type);
    last_type_.emplace(payload_type);
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
    if (!member_type.is_poison()) {
        record_member_owner(id, *implicit_type, implicit_access.member);
    }
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

auto type_resolver::visit(ast::node_id id, const ast::i32_expr& expr) -> void {
    PROFILE_FUNCTION();
    auto* resolved_type{&ctx_.get_builtin_resolved_type(type_kind::I32)};
    if (expr.value >= 0) {
        if (const auto implicit_type{implicit_type_stack_.peek()}) {
            const auto kind{implicit_type->get_kind()};
            if (is_integer(kind)) { resolved_type = implicit_type.get(); }
        }
    }
    last_type_.emplace(*resolved_type);
    last_type_->resolve_if<types::builtin_type>();
    resolving_.set_sema_type(id, *last_type_);
}

MAKE_PRIMITIVE_RESOLVER(i64_expr, I64)
MAKE_PRIMITIVE_RESOLVER(isize_expr, ISIZE)
MAKE_PRIMITIVE_RESOLVER(u32_expr, U32)
MAKE_PRIMITIVE_RESOLVER(u64_expr, U64)
MAKE_PRIMITIVE_RESOLVER(usize_expr, USIZE)
MAKE_PRIMITIVE_RESOLVER(u8_expr, U8)
MAKE_PRIMITIVE_RESOLVER(bool_expr, BOOL)
MAKE_PRIMITIVE_RESOLVER(void_expr, VOID_)
MAKE_PRIMITIVE_RESOLVER(undefined_expr, UNDEFINED)
MAKE_PRIMITIVE_RESOLVER(nullptr_expr, NULLPTR)
MAKE_PRIMITIVE_RESOLVER(unreachable_expr, NORETURN)
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
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    fmt::format("Module '{}' failed to resolve due to errors it contains",
                                get_rightmost_name(access.outer).value_or("<module>")),
                    error::IMPORTED_MODULE_CONTAINS_ERRORS,
                    resolving_.ast.location_of(access.outer)));
            }
        }

        // Step into the module's scope for lookup
        const auto& inner_ident{resolving_.ast.get_as<ast::identifier_expr>(access.inner)};
        auto        sym{ctx_.registry.get_from_opt(*inner_mod.root_table_idx, inner_ident.name)};
        if (!sym) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("Module '{}' has no member named '{}'",
                            get_rightmost_name(access.outer).value_or("<expression>"),
                            inner_ident.name),
                error::UNDECLARED_IDENTIFIER,
                resolving_.ast.location_of(access.inner)));
        }

        const auto symbol_node{sym->get_data().as_opt<symbols::node_t>()};
        if (!symbol_node) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }

        if (&inner_mod != &resolving_ && !sym->is_public(inner_mod)) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("Symbol '{}' is private to module '{}'",
                            inner_ident.name,
                            get_rightmost_name(access.outer).value_or("<expression>")),
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

        // Record where this cross-module reference resolves to, for LSP go-to-definition
        resolving_.set_identifier_definition(access.inner,
                                             {inner_mod.path, sym->get_symbol_span(inner_mod)});
        resolving_.add_identifier_position(access.inner);

        if constexpr (std::same_as<ID, ast::node_id>) {
            record_symbol_owner(id, *inner_mod.root_table_idx, inner_mod, *sym);
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

[[nodiscard]] auto extern_reference_field(std::string_view       kind,
                                          std::string_view       name,
                                          const source_location& location) -> diagnostic {
    return diagnostic{
        fmt::format("extern {} field '{}' cannot have a reference type; an 'extern' aggregate "
                    "has no ABI representation for references, use a raw pointer ('^T') instead",
                    kind,
                    name),
        error::ILLEGAL_REFERENCE_FIELD,
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

        sym->set_status(symbol_status::RESOLVING);
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

        if (struct_expr.is_extern && field_type->get_kind() == type_kind::REFERENCE) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                extern_reference_field(
                    "struct", ident.name, resolving_.ast.location_of(field.explicit_type))));
        }

        resolving_.set_sema_type(field.name, *field_type);
        sym->set_kind(symbol_kind::VALUE);
        sym->set_status(symbol_status::RESOLVED);
        field_types[i++] = field_type;
    }

    auto member_types{ctx_.pool.get_many_unsafe(struct_expr.members.size())};
    auto field_alignments{ctx_.arena.make_span<u64>(struct_expr.fields.size())};
    for (usize i{0}; const auto& field : struct_expr.fields) {
        u64 align_val{0};
        if (field.explicit_alignment) {
            gir::const_eval ce{ctx_, resolving_};
            const auto      cv{ce.try_eval(*field.explicit_alignment)};
            if (cv) {
                if (const auto val{cv->as_opt<u64>()}) {
                    align_val = *val;
                } else if (const auto sval{cv->as_opt<i64>()}) {
                    if (*sval > 0) { align_val = static_cast<u64>(*sval); }
                }
            }
        }
        field_alignments[i++] = align_val;
    }
    committable_resolution<types::struct_t> resolution{struct_type,
                                                       field_types,
                                                       struct_expr.fields,
                                                       member_types,
                                                       resolving_,
                                                       struct_expr.is_extern,
                                                       struct_expr.is_packed,
                                                       field_alignments};
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

        sym->set_status(symbol_status::RESOLVING);
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

        if (union_expr.is_extern && field_type.get_kind() == type_kind::REFERENCE) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                extern_reference_field(
                    "union", ident.name, resolving_.ast.location_of(field.explicit_type))));
        }

        resolving_.set_sema_type(field.name, field_type);
        sym->set_kind(symbol_kind::VALUE);
        sym->set_status(symbol_status::RESOLVED);
        field_types[i++] = &field_type;
    }

    auto member_types{ctx_.pool.get_many_unsafe(union_expr.members.size())};
    committable_resolution<types::union_t> resolution{
        union_type, field_types, union_expr.fields, member_types, resolving_, union_expr.is_extern};
    if (!resolve_members(member_types, union_expr.members)) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id));
    }

    resolution.commit();
    last_type_.emplace(union_type);
}

VISITOR_TEMPLATE_INIT(type_resolver, visit, const ast::union_expr&)

// Builds the `types::function` for a bodyless interface requirement
auto type_resolver::resolve_required_method_type(const ast::function_expr& fn,
                                                 type& self_placeholder) -> type& {
    const auto param_count{fn.parameters.size() + (fn.self ? 1UZ : 0UZ)};
    auto       params{ctx_.pool.get_many_unsafe(param_count)};
    usize      idx{0};

    if (fn.self) {
        type* self_ty{&self_placeholder};
        if (const auto mut{mutability_from_type_modifier(fn.self->modifier)}) {
            self_ty = fn.self->modifier.is_ptr() ? &ctx_.get_pointer(*mut, self_placeholder)
                                                 : &ctx_.get_reference(*mut, self_placeholder);
        }
        params[idx++] = self_ty;
    }
    for (const auto& param : fn.parameters) {
        resolve(param.explicit_type);
        params[idx++] = &denoted_type(*last_type_.take());
    }
    resolve(fn.explicit_return_type);
    auto& return_type{denoted_type(*last_type_.take())};

    types::key_t key{type_kind::FUNCTION, types::mut::CONSTANT};
    for (const auto* p : params) { key.imprint(*p); }
    key.imprint(return_type);
    auto& fn_type{*ctx_.pool[key]};
    fn_type.resolve_if<types::function>(params, return_type, fn.self.has_value(), fn.variadic);
    return fn_type;
}

template <ast::IndexableID ID>
auto type_resolver::visit(ID id, const ast::interface_expr& iface) -> void {
    PROFILE_FUNCTION();
    auto& iface_type{resolving_.get_sema_type(id)};

    const auto method_count{iface.methods.size()};
    auto       method_sigs{ctx_.arena.make_span<type*>(method_count)};
    auto       method_src{ctx_.arena.make_span<usize>(method_count)};
    auto       method_names{ctx_.arena.make_span<std::string_view>(method_count)};
    auto       method_is_pub{ctx_.arena.make_span<bool>(method_count)};

    {
        const scope            s{table_stack_, iface_type.get_symbol_table_idx(), table_idx_};
        const structural_guard g{user_type_stack_, iface_type};

        for (const auto& at : iface.assoc_types) {
            TRY_RESOLVE(at.annotation);
            auto& annotation{*last_type_.take()};
            if (annotation.get_kind() != type_kind::TYPE && !annotation.is_poison()) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    fmt::format("associated type `{}` must be annotated `: type`",
                                resolving_.ast.get_as<ast::identifier_expr>(*at.name).name),
                    error::TYPE_MISMATCH,
                    resolving_.ast.location_of(at.annotation)));
            }
            resolving_.set_sema_type(at.name, annotation);
            resolve_symbol_info(at.name, symbol_kind::TYPE);
            if (at.default_type) { TRY_RESOLVE(*at.default_type); }
        }

        for (const auto& ac : iface.assoc_consts) {
            TRY_RESOLVE(ac.explicit_type);
            auto& const_type{denoted_type(*last_type_.take())};
            resolving_.set_sema_type(ac.name, const_type);
            resolve_symbol_info(ac.name, symbol_kind::VALUE);
        }

        // Build the method signatures only (params / return / self) requirements-first, so
        // `requirement_count` partitions the arrays.
        usize      out{0};
        const auto emit_pass{[&](bool want_required) -> void {
            for (usize src{0}; src < iface.methods.size(); ++src) {
                const auto& m{iface.methods[src]};
                const auto& fn{resolving_.ast.get_as<ast::function_expr>(*m.signature)};
                if (fn.is_type_expr != want_required) { continue; }

                method_sigs[out]   = &resolve_required_method_type(fn, iface_type);
                method_src[out]    = src;
                method_names[out]  = resolving_.ast.get_as<ast::identifier_expr>(*m.name).name;
                method_is_pub[out] = m.is_public();
                ++out;
            }
        }};
        emit_pass(true);

        // Default-method *bodies* are resolved after the interface type is committed, so
        // `self.method(...)` inside them can see the set.
        const auto requirement_count{out};
        emit_pass(false);

        auto assoc_type_names{ctx_.arena.make_span<std::string_view>(iface.assoc_types.size())};
        for (usize i{0}; const auto& at : iface.assoc_types) {
            assoc_type_names[i++] = resolving_.ast.get_as<ast::identifier_expr>(*at.name).name;
        }
        auto assoc_const_names{ctx_.arena.make_span<std::string_view>(iface.assoc_consts.size())};
        for (usize i{0}; const auto& ac : iface.assoc_consts) {
            assoc_const_names[i++] = resolving_.ast.get_as<ast::identifier_expr>(*ac.name).name;
        }

        committable_resolution<types::interface_t> resolution{iface_type,
                                                              method_sigs,
                                                              method_src,
                                                              method_names,
                                                              method_is_pub,
                                                              requirement_count,
                                                              assoc_type_names,
                                                              assoc_const_names,
                                                              iface.methods,
                                                              iface.assoc_types,
                                                              iface.assoc_consts,
                                                              resolving_};
        resolution.commit();

        // With the interface type committed, default-method bodies and defaulted associated
        // items can now resolve `self.method(...)` / `Self`-typed references against the set.
        for (const auto& at : iface.assoc_types) {
            if (at.default_type) { TRY_RESOLVE(*at.default_type); }
        }
        for (const auto& ac : iface.assoc_consts) {
            if (ac.default_value) {
                const structural_guard inner{implicit_type_stack_,
                                             resolving_.get_sema_type(ac.name)};
                TRY_RESOLVE(*ac.default_value);
            }
        }
        for (const auto& m : iface.methods) {
            const auto& fn{resolving_.ast.get_as<ast::function_expr>(*m.signature)};
            if (!fn.is_type_expr) {
                resolve(m.signature);
                DISCARD(last_type_.take());
            }
        }
    }

    last_type_.emplace(iface_type);
}

VISITOR_TEMPLATE_INIT(type_resolver, visit, const ast::interface_expr&)

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
    resolving_.set_sema_type(
        id, loop_type.is_poison() ? ctx_.get_builtin_resolved_type(type_kind::VOID_) : loop_type);
    last_type_.emplace(resolving_.get_sema_type(id));
}

// DONT CALL ME FROM ANY LOOP/CONDITION/FN RESOLVER
auto type_resolver::visit(ast::node_id id, const ast::block_stmt& block) -> void {
    PROFILE_FUNCTION();
    auto&       block_type{resolving_.get_sema_type(id)};
    const scope s{table_stack_, block_type.get_symbol_table_idx(), table_idx_};

    // Just an abridged loop handler
    for (const auto& stmt : block) { TRY_RESOLVE(stmt); }
    resolving_.set_sema_type(
        id, block_type.is_poison() ? ctx_.get_builtin_resolved_type(type_kind::VOID_) : block_type);
    last_type_.emplace(resolving_.get_sema_type(id));
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
            auto& void_type{ctx_.get_builtin_resolved_type(type_kind::VOID_)};
            label_data.add_yield_type(void_type);
            resolving_.set_sema_type(id, void_type);
        }
    } else {
        resolving_.set_sema_type(id, ctx_.get_builtin_resolved_type(type_kind::VOID_));
    }

    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::NORETURN));
}

auto type_resolver::visit(ast::node_id id, const ast::continue_stmt& continue_stmt) -> void {
    PROFILE_FUNCTION();
    auto result{resolve_control_flow_label(continue_stmt.label, "continue")};
    if (!result) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(result).error()));
    }

    resolving_.set_sema_type(id, ctx_.get_builtin_resolved_type(type_kind::VOID_));
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
        auto& void_type{ctx_.get_builtin_resolved_type(type_kind::VOID_)};
        // `id` may not be the node that resolved this symbol
        if (!resolving_.has_sema_type(id)) {
            resolving_.set_sema_type(decl.name, void_type);
            resolving_.set_sema_type(id, void_type);
        }
        return last_type_.emplace(void_type);
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
            auto* explicit_type_p{&denoted_type(*last_type_.take())};
            // `const x: MakesAType() = ...`
            if (explicit_type_p->get_data().is<types::deferred_call>()) {
                const auto& dc_call{explicit_type_p->get_data().as<types::deferred_call>().call};
                gir::const_eval evaluator{ctx_, resolving_};
                explicit_type_p = &evaluator.force_deferred_call(*explicit_type_p);
                register_non_generic_type_ctor_members(*explicit_type_p, dc_call);
            }
            auto& explicit_type{*explicit_type_p};
            if (explicit_type.get_kind() == type_kind::INTERFACE) {
                const auto iname{ctx_.type_display_name(explicit_type)};
                ctx_.poison_symbol(
                    sym,
                    fmt::format("`{}` is an interface and cannot be stored by value; use "
                                "`&dyn {}`, `^dyn {}`, or an `impl {}` parameter",
                                iname,
                                iname,
                                iname,
                                iname),
                    error::INTERFACE_NOT_A_VALUE,
                    resolving_.ast.location_of(*decl.explicit_type));
                resolving_.set_sema_type(decl.name, ctx_.get_poison());
                return last_type_.emplace(ctx_.poison_node(resolving_, id));
            }
            if (explicit_type.get_kind() == type_kind::AUTO) {
                // `undefined` carries no type, so it cannot drive `auto` inference either.
                const bool undef_init{decl.value &&
                                      resolving_.ast.get_as_opt<ast::undefined_expr>(*decl.value)};
                if (!decl.value || undef_init) {
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
        if (decl.has_modifier(ast::decl_modifiers::DISCARDABLE)) {
            const auto fn_d{type_data.as_opt<types::function>()};
            if (!fn_d && !type_data.is<types::builtin_function>()) {
                ctx_.diags.emplace_back("'@discardable' may only be applied to a function",
                                        error::ILLEGAL_DISCARDABLE,
                                        resolving_.ast.location_of(id));
            } else if (fn_d && fn_d->return_type.get_kind() == type_kind::VOID_) {
                ctx_.diags.emplace_back(
                    "'@discardable' has no effect on a function that returns 'void'",
                    error::ILLEGAL_DISCARDABLE,
                    resolving_.ast.location_of(id));
            }
        }

        const bool literal_type_anno{decl.explicit_type && decl.explicit_type->get_token_type() ==
                                                               syntax::token_type_t::TYPE_TYPE};
        if (decl.has_modifier(ast::decl_modifiers::VARIABLE) && literal_type_anno) {
            ctx_.poison_symbol(sym,
                               "a 'type' value cannot be stored in a mutable ('var') binding; "
                               "use 'const', 'constexpr', or 'using' instead",
                               error::MUTABLE_TYPE_BINDING,
                               resolving_.ast.location_of(id));
            resolving_.set_sema_type(decl.name, ctx_.get_poison());
            return last_type_.emplace(ctx_.poison_node(resolving_, id));
        }

        if (!sym.has_kind()) {
            if (type_data.is<types::builtin_function>() || type_data.is<types::function>()) {
                sym.set_kind(symbol_kind::CALLABLE);
            } else if (resolved_type == ctx_.get_builtin_resolved_type(type_kind::TYPE)) {
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

        // Remember the declared name of a struct/enum/union so `@typeName` can report it
        if (decl.value && decl.value->any<ast::struct_expr, ast::union_expr, ast::enum_expr>()) {
            if (const auto value_type{resolving_.get_sema_type_opt(*decl.value)};
                value_type && !value_type->is_poison()) {
                const auto& decl_ident{resolving_.ast.get_as<ast::identifier_expr>(decl.name)};
                ctx_.user_type_names.try_emplace(value_type.get(), decl_ident.name);
            }
        }
    }

    resolving_.set_sema_type_if(decl.name, resolved_type);
    sym.set_status(symbol_status::RESOLVED);
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_));
}

auto type_resolver::visit(ast::node_id id, const ast::defer_stmt& defer) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(defer.deferred);
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_));
}

auto type_resolver::visit(ast::node_id id, const ast::discard_stmt& discard) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(discard.discarded);
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_));
}

auto type_resolver::callee_is_discardable(const ast::call_expr& call) const -> bool {
    const mod::module* home{&resolving_};
    ast::node_id       fn_node{*call.function};

    for (int hops{0}; hops < 16; ++hops) {
        stdx::option<symbol&> sym;
        if (const auto ident{home->ast.get_as_opt<ast::identifier_expr>(fn_node)}) {
            sym = (home == &resolving_) ? ctx_.registry.lookup(table_stack_, ident->name)
                                        : stdx::option<symbol&>{};
            if (!sym && home->root_table_idx) {
                sym = ctx_.registry.get_from_opt(*home->root_table_idx, ident->name);
            }
        } else if (const auto mac{home->ast.get_as_opt<ast::module_access_expr>(fn_node)}) {
            const auto outer_type{home->get_sema_type_opt(mac->outer)};
            const auto m_data{outer_type ? outer_type->get_data().as_opt<types::module>()
                                         : stdx::none};
            if (!m_data || !m_data->imported.root_table_idx) { return false; }
            const auto& inner_ident{home->ast.get_as<ast::identifier_expr>(mac->inner)};
            sym  = ctx_.registry.get_from_opt(*m_data->imported.root_table_idx, inner_ident.name);
            home = &m_data->imported;
        } else {
            return false;
        }
        if (!sym) { return false; }

        const auto node{sym->get_data().as_opt<symbols::node_t>()};
        if (!node) { return false; }
        const auto decl{home->ast.get_as_opt<ast::decl_stmt>(*node)};
        if (!decl) { return false; }
        if (decl->has_modifier(ast::decl_modifiers::DISCARDABLE)) { return true; }

        // Follow a direct `const g := f` / `const g := m::f` re-export to the real declaration.
        if (decl->value && (home->ast.get_as_opt<ast::identifier_expr>(*decl->value) ||
                            home->ast.get_as_opt<ast::module_access_expr>(*decl->value))) {
            fn_node = *decl->value;
            continue;
        }
        return false;
    }
    return false;
}

auto type_resolver::check_unused_result(ast::node_id stmt_id, const ast::expr_stmt& stmt) -> void {
    const auto value_type{resolving_.get_sema_type_opt(stmt.expression)};
    if (!value_type || value_type->is_poison()) { return; }
    switch (value_type->get_kind()) {
    case type_kind::VOID_:
    case type_kind::NORETURN:
    case type_kind::AUTO:     return;
    default:                  break;
    }

    // Only a call whose result is thrown away is flagged; a bare `a + b;` is left alone for now.
    const auto call{resolving_.ast.get_as_opt<ast::call_expr>(*stmt.expression)};
    const auto unwrap{resolving_.ast.get_as_opt<ast::unwrap_expr>(*stmt.expression)};
    if (!call && !unwrap) { return; }
    if (call) {
        // `@builtin(...)` calls (`@expect`, `@memcpy`, …) are their own category, not covered.
        if (const auto fn_ty{resolving_.get_sema_type_opt(call->function)};
            fn_ty && fn_ty->get_data().is<types::builtin_function>()) {
            return;
        }
        if (callee_is_discardable(*call)) { return; }
    }

    ctx_.diags.emplace_back(
        "result of this call is unused; bind it, pass it on, `_ =` it, or mark the callee "
        "'@discardable'",
        error::UNUSED_RESULT,
        resolving_.ast.location_of(stmt_id));
}

auto type_resolver::visit(ast::node_id id, const ast::expr_stmt& expr) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(expr.expression);
    resolving_.set_sema_type(expr.expression, *last_type_.take());
    check_unused_result(id, expr);
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_));
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
    const auto mod_opt{import_type.get_data().as_opt<types::module>()};
    if (!mod_opt) { return poison_out(); }
    auto& module{*mod_opt};

    // There's no need to poison the import type since it would lose all of the module information
    context new_ctx{ctx_};
    resolve_types(module.imported, new_ctx);
    if (module.imported.is_poisoned()) {
        last_type_.emplace(ctx_.get_poison());
        resolving_.set_sema_type(ident_id, *last_type_);
        return ctx_.poison_symbol(
            sym,
            fmt::format("Import '{}' failed to resolve due to errors it contains", name),
            error::IMPORTED_MODULE_CONTAINS_ERRORS,
            resolving_.ast.location_of(id));
    }

    // Give the alias identifier the module type so the LSP can hover over it
    resolving_.set_sema_type_if(ident_id, import_type);
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_));
}

auto type_resolver::visit(ast::node_id id, const ast::return_stmt& return_stmt) -> void {
    PROFILE_FUNCTION();
    if (return_stmt.expression) {
        // Make the declared return type the implicit type for the returned expression
        stdx::option<structural_guard> return_type_guard;
        if (!return_trackers_.empty() && return_trackers_.back().expected_type) {
            return_type_guard.emplace(implicit_type_stack_, *return_trackers_.back().expected_type);
        }

        TRY_RESOLVE(*return_stmt.expression);
        auto& return_expr_type{*last_type_.take()};

        // Only these two shapes are checked; the AST can't tell origin from pass-through.
        const auto constructs_closure_literal = [&] {
            if (resolving_.ast.get_as_opt<ast::function_expr>(*return_stmt.expression)) {
                return true;
            }
            const auto ident{
                resolving_.ast.get_as_opt<ast::identifier_expr>(*return_stmt.expression)};
            if (!ident) { return false; }
            const auto sym{ctx_.registry.lookup(table_stack_, ident->name)};
            if (!sym) { return false; }
            const auto node{sym->get_data().as_opt<symbols::node_t>()};
            if (!node) { return false; }
            const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*node)};
            return decl && decl->value &&
                   resolving_.ast.get_as_opt<ast::function_expr>(*decl->value);
        }();

        if (constructs_closure_literal) {
            if (const auto cl{return_expr_type.get_data().as_opt<types::closure_t>()};
                cl && has_dangling_capture(*cl)) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    "Closure captures a reference into its enclosing function and cannot be "
                    "returned directly; mark it 'move fn' to capture by value instead",
                    error::ILLEGAL_CLOSURE_ESCAPE,
                    resolving_.ast.location_of(*return_stmt.expression)));
            }
        }
        resolving_.set_sema_type(id, return_expr_type);
        if (!return_trackers_.empty()) { return_trackers_.back().add_return(return_expr_type); }
    } else {
        auto& void_type{ctx_.get_builtin_resolved_type(type_kind::VOID_)};
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
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_));
}

// Resolves an `impl` target/interface reference and, if it names an as-yet-unresolved aggregate,
// forces that aggregate's declaration to resolve so the impl registry can key on final identity.
auto type_resolver::resolve_impl_type_ref(ast::explicit_type_id ref) -> type& {
    resolve(ref);
    auto&      t{denoted_type(*last_type_.take())};
    const auto k{t.get_kind()};
    if (!is_aggregate(k) || t.is_resolved()) { return t; }

    // A forward reference to a not-yet-resolved aggregate: drive its decl to completion.
    if (const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(ref)}) {
        if (const auto lookup{ctx_.registry.lookup(table_stack_, ident->name)}) {
            if (const auto node{lookup->get_data().as_opt<symbols::node_t>()}) {
                if (const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*node)};
                    decl && decl->value) {
                    resolve(*decl->value);
                    last_type_.reset();
                }
            }
        }
    }
    return t;
}

auto type_resolver::pre_register_impls() -> void {
    PROFILE_FUNCTION();
    for (const auto root : resolving_.ast) {
        const auto impl{resolving_.ast.get_as_opt<ast::impl_stmt>(root)};
        if (!impl) { continue; }
        if (!impl->impl_params.empty()) {
            register_parameterized_impl(root, *impl);
            continue;
        }

        auto&       impl_type{resolving_.get_sema_type(root)};
        const scope s{table_stack_, impl_type.get_symbol_table_idx(), table_idx_};

        stdx::option<const type&> interface_type;
        if (impl->interface_type) {
            auto& it{resolve_impl_type_ref(*impl->interface_type)};
            if (it.is_poison()) { continue; }
            interface_type.emplace(it);
        }
        auto& target{resolve_impl_type_ref(impl->target_type)};
        if (target.is_poison()) { continue; }

        // A trait impl is legal in the module that declares `I` or `T`
        stdx::option<const mod::module&> here{resolving_};
        const auto iface_mod{interface_type ? declaring_module_of(*interface_type) : stdx::none};
        const auto target_mod{declaring_module_of(target)};
        if (interface_type) {
            if (iface_mod != here && target_mod != here) {
                ctx_.diags.emplace_back(
                    "`impl I for T` is only allowed in the module that declares `I` or `T`",
                    error::ORPHAN_IMPL,
                    resolving_.ast.location_of(root));
                continue;
            }
        } else if (target_mod != here) {
            ctx_.diags.emplace_back(
                "an inherent `impl T` requires `T` to be declared in this module",
                error::ORPHAN_IMPL,
                resolving_.ast.location_of(root));
            continue;
        }

        impl_record rec{
            .interface_type = interface_type,
            .target_type    = target,
            .site           = root,
            .enclosing      = here,
            .body_scope_idx = impl_type.get_symbol_table_idx(),
        };
        for (const auto& member : impl->members) {
            const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*member)};
            if (!decl || !decl->value || !decl->value->is<ast::function_expr>()) { continue; }
            rec.methods.emplace_back<impl_record::method>({
                .name    = resolving_.ast.get_as<ast::identifier_expr>(*decl->name).name,
                .decl    = *member,
                .fn_type = stdx::none,
                .is_pub  = decl->has_modifier(ast::decl_modifiers::PUBLIC),
            });
        }

        // An inherent `impl T` member may not shadow a native member of `T` or a member
        // already added by another inherent `impl T` block.
        if (!interface_type && target.has_symbol_table_idx()) {
            const auto& native{ctx_.registry.get(target.get_symbol_table_idx())};
            for (const auto& m : rec.methods) {
                bool clashes{native.has(m.name)};
                for (const auto* other : ctx_.impls.records()) {
                    if (other->interface_type || !other->target_type ||
                        other->target_type != &target) {
                        continue;
                    }
                    clashes = clashes || other->find_method(m.name).has_value();
                }

                if (clashes) {
                    ctx_.diags.emplace_back(
                        fmt::format("`{}` is already a member of `{}`; an inherent `impl` may not "
                                    "redefine it",
                                    m.name,
                                    ctx_.type_display_name(target)),
                        error::DUPLICATE_MEMBER,
                        resolving_.ast.location_of(m.decl));
                }
            }
        }

        auto recorded{ctx_.impls.record(std::move(rec))};
        if (!recorded) {
            const auto prior_loc{resolving_.ast.location_of(recorded.error())};
            ctx_.diags.emplace_back(
                fmt::format("duplicate `impl` for this type; previous impl here: {}", prior_loc),
                error::DUPLICATE_IMPL,
                resolving_.ast.location_of(root));
        }
    }
}

namespace {

// The bare identifier naming a `call_expr` argument, if it is one (`Ctor(T)` → "T" at slot 0).
[[nodiscard]] auto ctor_arg_ident(const mod::module& mod, const ast::call_expr::argument& arg)
    -> stdx::option<std::string_view> {
    return arg.visit(
        [&](ast::expr_handle h) -> stdx::option<std::string_view> {
            if (const auto id{mod.ast.get_as_opt<ast::identifier_expr>(*h)}) { return id->name; }
            return stdx::none;
        },
        [&](ast::explicit_type_id t) -> stdx::option<std::string_view> {
            if (const auto id{mod.ast.get_as_opt<ast::identifier_expr>(t)}) { return id->name; }
            return stdx::none;
        });
}

} // namespace

auto type_resolver::param_impl_sentinel(usize disc) -> type& {
    auto& s{*ctx_.pool[{type_kind::TYPE,
                        types::mut::CONSTANT,
                        std::string_view{"pimpl.sentinel"},
                        static_cast<u64>(disc)}]};
    if (!s.is_resolved()) { s.resolve<types::builtin_type>(); }
    return s;
}

auto type_resolver::register_parameterized_impl(ast::node_id root, const ast::impl_stmt& impl)
    -> void {
    PROFILE_FUNCTION();
    auto&       impl_type{resolving_.get_sema_type(root)};
    const scope s{table_stack_, impl_type.get_symbol_table_idx(), table_idx_};

    // Peel `Ctor(<impl params>)` down to `Ctor` and note which arg slot each impl param fills.
    const auto tgt_call{resolving_.ast.get_as_opt<ast::call_expr>(impl.target_type)};
    std::vector<stdx::option<std::string_view>> ctor_arg_names;
    if (tgt_call) {
        for (const auto& a : tgt_call->arguments) {
            ctor_arg_names.emplace_back(ctor_arg_ident(resolving_, a));
        }
    }

    // Resolve the base ctor identifier to its generic `function_expr` node + declaring module.
    stdx::option<const mod::module&> base_mod;
    stdx::option<ast::node_id>       base_ctor_fn;
    if (tgt_call) {
        if (const auto id{resolving_.ast.get_as_opt<ast::identifier_expr>(*tgt_call->function)}) {
            if (const auto sym{ctx_.registry.lookup(table_stack_, id->name)}) {
                if (const auto n{sym->get_data().as_opt<symbols::node_t>()}) {
                    if (const auto d{resolving_.ast.get_as_opt<ast::decl_stmt>(*n)};
                        d && d->value) {
                        base_mod.emplace(resolving_);
                        base_ctor_fn.emplace(*d->value);
                    }
                }
            }
        } else if (const auto mac{
                       resolving_.ast.get_as_opt<ast::module_access_expr>(*tgt_call->function)}) {
            resolve(mac->outer); // the module alias is not otherwise typed this early
            if (const auto mod_type{resolving_.get_sema_type_opt(mac->outer)}) {
                if (const auto md{mod_type->get_data().as_opt<types::module>()};
                    md && md->imported.root_table_idx) {
                    const auto& inner{resolving_.ast.get_as<ast::identifier_expr>(mac->inner)};
                    if (const auto sym{
                            ctx_.registry.get_from_opt(*md->imported.root_table_idx, inner.name)}) {
                        if (const auto n{sym->get_data().as_opt<symbols::node_t>()}) {
                            if (const auto d{md->imported.ast.get_as_opt<ast::decl_stmt>(*n)};
                                d && d->value) {
                                base_mod.emplace(md->imported);
                                base_ctor_fn.emplace(*d->value);
                            }
                        }
                    }
                }
            }
        }
    }
    if (!base_mod || !base_ctor_fn) { return; } // cannot anchor: skip quietly

    stdx::option<const type&> iface_type;
    if (impl.interface_type) {
        auto& it{resolve_impl_type_ref(*impl.interface_type)};
        if (it.is_poison()) { return; }
        iface_type.emplace(it);
    }

    // The base ctor OR the interface must be declared in this module.
    const auto iface_mod{iface_type ? declaring_module_of(*iface_type) : stdx::none};
    if (base_mod != &resolving_ && iface_mod != &resolving_) {
        ctx_.diags.emplace_back(
            "a parameterized `impl` must be anchored in this module: its base type constructor "
            "or its interface must be declared here",
            error::ORPHAN_IMPL,
            resolving_.ast.location_of(root));
        return;
    }

    std::vector<stdx::opt_size> mapping;
    mapping.reserve(impl.impl_params.size());
    for (const auto& p : impl.impl_params) {
        stdx::opt_size slot;
        if (const auto pn{resolving_.ast.get_as_opt<ast::identifier_expr>(p.name)}) {
            for (usize i{0}; i < ctor_arg_names.size(); ++i) {
                if (ctor_arg_names[i] && *ctor_arg_names[i] == pn->name) {
                    slot.emplace(i);
                    break;
                }
            }
        }
        mapping.emplace_back(slot);
    }

    stdx::option<const mod::module&> here{resolving_};
    ctx_.impls.record_parameterized(parameterized_impl{
        .site              = root,
        .interface_type    = iface_type,
        .base_ctor_fn      = *base_ctor_fn,
        .enclosing         = here,
        .body_scope_idx    = impl_type.get_symbol_table_idx(),
        .param_to_ctor_arg = std::move(mapping),
    });
}

auto type_resolver::instantiate_impls_for(
    type&                                                     concrete,
    const mod::module&                                        base_mod,
    ast::node_id                                              base_ctor_fn,
    gsl::span<type* const>                                    ctor_args,
    gsl::span<const std::pair<std::string, gir::const_value>> ctor_cx,
    const ast::function_expr&                                 base_fn,
    std::string_view                                          ctor_mangled) -> void {
    PROFILE_FUNCTION();
    for (auto* pimpl : ctx_.impls.param_records()) {
        if (pimpl->base_ctor_fn.get_index() != base_ctor_fn.get_index() ||
            pimpl->base_ctor_fn.get_kind() != base_ctor_fn.get_kind()) {
            continue;
        }
        if (!pimpl->enclosing) { continue; }
        auto&      impl_mod{const_cast<mod::module&>(*pimpl->enclosing)};
        const auto impl_stmt{impl_mod.ast.get_as_opt<ast::impl_stmt>(pimpl->site)};
        if (!impl_stmt) { continue; }

        // Bind each impl param: a type param -> the concrete ctor argument type
        std::vector<type*> type_bounds(pimpl->param_to_ctor_arg.size(), nullptr);
        std::vector<std::pair<std::string, gir::const_value>> cx_bindings;
        bool                                                  ok{true};
        for (usize i{0}; i < pimpl->param_to_ctor_arg.size(); ++i) {
            const auto slot{pimpl->param_to_ctor_arg[i]};
            if (!slot || *slot >= ctor_args.size() || *slot >= base_fn.parameters.size()) {
                ok = false;
                break;
            }
            if (i < impl_stmt->impl_params.size() && impl_stmt->impl_params[i].is_constexpr) {
                const auto& cx_name{
                    base_mod.ast.get_as<ast::identifier_expr>(base_fn.parameters[*slot].name).name};
                const auto it{std::ranges::find(
                    ctor_cx, cx_name, [](const auto& p) { return std::string_view{p.first}; })};
                if (it == ctor_cx.end()) {
                    ok = false;
                    break;
                }

                cx_bindings.emplace_back(
                    std::string{
                        impl_mod.ast.get_as<ast::identifier_expr>(impl_stmt->impl_params[i].name)
                            .name},
                    it->second);
            } else {
                auto* arg{ctor_args[*slot]};
                // A still-abstract `type` argument means this is not a real monomorphization.
                if (!arg || arg->get_kind() == type_kind::TYPE || arg->is_poison()) {
                    ok = false;
                    break;
                }
                if (i < type_bounds.size()) { type_bounds[i] = arg; }
            }
        }
        if (!ok) { continue; }

        // The impl's own module may not have run its template pass yet (a consumer reached this
        // ctor first). Build it now against that module so expansion is order-independent.
        auto tmpl_p{ctx_.impls.get_template(*pimpl)};
        if (!tmpl_p) {
            if (&impl_mod == &resolving_) {
                const scope s{table_stack_,
                              resolving_.get_sema_type(pimpl->site).get_symbol_table_idx(),
                              table_idx_};
                build_param_impl_template(*impl_stmt, pimpl->site);
            } else {
                symbol_table_stack stk;
                stk.push(*ctx_.prelude_index);
                if (impl_mod.root_table_idx) { stk.push(*impl_mod.root_table_idx); }
                stk.push(pimpl->body_scope_idx);
                type_resolver sub{impl_mod, ctx_, pimpl->body_scope_idx, std::move(stk)};
                sub.for_generic_instantiation_ = true;
                sub.build_param_impl_template(*impl_stmt, pimpl->site);
            }
            tmpl_p = ctx_.impls.get_template(*pimpl);
        }
        if (!tmpl_p) { continue; }
        const auto& tmpl{*tmpl_p};
        if (!tmpl.abstract_target || tmpl.sentinels.size() != type_bounds.size()) { continue; }

        // One expansion per (parameterized impl, concrete target).
        if (!ctx_.impls.mark_expanded(concrete, *pimpl)) { continue; }

        const auto remap_one{[&](type* t) -> type* {
            type* r{&remap_type(ctx_, *t, *tmpl.abstract_target, concrete)};
            for (usize i{0}; i < tmpl.sentinels.size() && i < type_bounds.size(); ++i) {
                if (tmpl.sentinels[i] && type_bounds[i]) {
                    r = &remap_type(ctx_, *r, *tmpl.sentinels[i], *type_bounds[i]);
                }
            }
            return r;
        }};

        // Build this monomorphization's typing by remapping the template.
        body_type_diff typing;
        for (const auto& [idx, ty] : tmpl.node_types) {
            typing.node_types.emplace_back(idx, remap_one(ty));
        }
        for (const auto& [idx, ty] : tmpl.explicit_types) {
            typing.explicit_types.emplace_back(idx, remap_one(ty));
        }

        const auto typing_key{fmt::format("pimpl{}#{}", pimpl->site.get_index(), ctor_mangled)};
        if (!typing.empty()) {
            ctx_.instantiation_cache.set_body_type_diff(typing_key, std::move(typing));
        }
        if (!cx_bindings.empty()) {
            ctx_.instantiation_cache.set_type_ctor_bindings(typing_key, std::move(cx_bindings));
        }

        impl_record rec{
            .interface_type     = pimpl->interface_type,
            .target_type        = concrete,
            .site               = pimpl->site,
            .enclosing          = pimpl->enclosing,
            .body_scope_idx     = pimpl->body_scope_idx,
            .from_parameterized = true,
            .gir_prefix         = typing_key,
        };
        for (const auto& member : impl_stmt->members) {
            const auto decl{impl_mod.ast.get_as_opt<ast::decl_stmt>(*member)};
            if (!decl || !decl->value || !decl->value->is<ast::function_expr>()) { continue; }
            impl_record::method m{
                .name    = impl_mod.ast.get_as<ast::identifier_expr>(*decl->name).name,
                .decl    = *member,
                .fn_type = stdx::none,
                .is_pub  = decl->has_modifier(ast::decl_modifiers::PUBLIC),
            };
            if (const auto t{impl_mod.get_sema_type_opt(*decl->value)}) {
                m.fn_type = remap_one(const_cast<type*>(t.get()));
            }
            rec.methods.emplace_back(std::move(m));
        }

        const bool trait{pimpl->interface_type.has_value()};
        auto       recorded{ctx_.impls.record(std::move(rec))};
        if (!recorded) {
            ctx_.diags.emplace_back(
                fmt::format("parameterized `impl` yields a duplicate for this instantiation; "
                            "previous impl here: {}",
                            impl_mod.ast.location_of(recorded.error())),
                error::DUPLICATE_IMPL,
                impl_mod.ast.location_of(pimpl->site));
            continue;
        }

        auto* stored{recorded->get()};
        if (trait && stored->interface_type) {
            if (const auto iface{stored->interface_type->get_data().as_opt<types::interface_t>()}) {
                check_impl_conformance(*stored, *iface);
            }
        }

        for (const auto& m : stored->methods) {
            impl_mod.impl_ctor_member_emits.emplace_back<type_ctor_member_emit>({
                .owner_clone = &concrete,
                .member_decl = m.decl,
                .gir_name    = fmt::format("{}.{}", stored->gir_prefix, m.name),
                .typing_key  = typing_key,
            });
        }
    }
}

auto type_resolver::build_param_impl_template(const ast::impl_stmt& impl, ast::node_id site)
    -> void {
    PROFILE_FUNCTION();
    // `pimpl` is absent for an unanchored `impl(P) BareType` (no base ctor to key on). We still
    // resolve the body here so its symbols settle; there is just nothing to expand later.
    const auto pimpl{ctx_.impls.param_record_by_site(site, resolving_)};
    if (pimpl) {
        if (ctx_.impls.get_template(*pimpl)) { return; }
        // Resolving the abstract `Ctor(...)` target below re-enters expansion for this same
        // impl; bail on that recursive entry. Cleared once the template is stored.
        if (!ctx_.impls.begin_build(*pimpl)) { return; }
    }

    // Bind each type param to its own opaque sentinel `type` and each `constexpr` param to a
    // dummy value, then resolve the target + members once
    std::vector<const type*> sentinels;
    sentinels.reserve(impl.impl_params.size());
    constexpr_frame cx_dummy;
    for (const auto& p : impl.impl_params) {
        if (p.is_constexpr) {
            resolve(p.explicit_type);
            auto& pty{last_type_ && !last_type_->is_poison()
                          ? denoted_type(*last_type_)
                          : ctx_.get_builtin_resolved_type(type_kind::USIZE)};
            resolving_.set_sema_type(p.name, pty);
            resolve_symbol_info(p.name, symbol_kind::VALUE);
            sentinels.emplace_back(nullptr);
            cx_dummy.insert_or_assign(resolving_.ast.get_as<ast::identifier_expr>(p.name).name,
                                      gir::const_value{u64{1}, pty});
        } else {
            auto& sentinel{param_impl_sentinel((*p.name).get_index())};
            resolving_.set_sema_type(p.name, sentinel);
            resolve_symbol_info(p.name, symbol_kind::TYPE);
            sentinels.emplace_back(&sentinel);
        }
    }
    const constexpr_frame_guard cx_guard{ctx_.constexpr_binding_frames, std::move(cx_dummy)};

    const auto snap_nodes{resolving_.sema_side_tables.node_types.values};
    const auto snap_types{resolving_.sema_side_tables.explicit_types.values};
    const auto diags_before{ctx_.diags.size()};

    if (impl.interface_type) { resolve(*impl.interface_type); }
    resolve(impl.target_type);
    stdx::option<const type&>      abstract_target;
    stdx::option<structural_guard> guard;
    if (last_type_ && !last_type_->is_poison()) {
        auto& t{denoted_type(*last_type_)};
        abstract_target.emplace(t);
        guard.emplace(user_type_stack_, t);
    }
    for (const auto& member : impl.members) { resolve(*member); }
    if (ctx_.diags.size() > diags_before) { DISCARD(ctx_.diags.split_off(diags_before)); }

    param_impl_template tmpl{.abstract_target = abstract_target, .sentinels = std::move(sentinels)};
    const auto          collect{[](const auto& live, const auto& snap, auto& out) {
        for (usize i{0}; i < live.size(); ++i) {
            if (!live[i]) { continue; }
            const bool changed{i >= snap.size() || !snap[i] || snap[i].get() != live[i].get()};
            if (changed) { out.emplace_back(i, live[i].get()); }
        }
    }};
    if (!pimpl) { return; } // unanchored: members resolved, nothing to store

    collect(resolving_.sema_side_tables.node_types.values, snap_nodes, tmpl.node_types);
    collect(resolving_.sema_side_tables.explicit_types.values, snap_types, tmpl.explicit_types);
    ctx_.impls.set_template(*pimpl, std::move(tmpl));
    ctx_.impls.end_build(*pimpl);
}

auto type_resolver::visit(ast::node_id id, const ast::impl_stmt& impl) -> void {
    PROFILE_FUNCTION();
    auto&       impl_type{resolving_.get_sema_type(id)};
    const scope s{table_stack_, impl_type.get_symbol_table_idx(), table_idx_};
    auto&       void_type{ctx_.get_builtin_resolved_type(type_kind::VOID_)};

    // Parameterized `impl(P) ...` is re-instantiated per monomorphization of its target.
    if (!impl.impl_params.empty()) {
        build_param_impl_template(impl, id);
        return last_type_.emplace(void_type);
    }

    const auto rec{ctx_.impls.find_by_site(id)};

    if (impl.interface_type) { TRY_RESOLVE(*impl.interface_type); }
    TRY_RESOLVE(impl.target_type);
    auto& target{denoted_type(*last_type_)};

    {
        stdx::option<structural_guard> guard;
        if (!target.is_poison()) { guard.emplace(user_type_stack_, target); }
        for (const auto& member : impl.members) { TRY_RESOLVE(*member); }
    }

    if (rec) {
        // Fill each impl method's resolved function type from its member decl.
        for (auto& m : rec->methods) {
            if (const auto t{resolving_.get_sema_type_opt(m.decl)}) { m.fn_type.emplace(*t); }
        }
        if (rec->interface_type != nullptr && !target.is_poison()) {
            if (const auto iface{rec->interface_type->get_data().as_opt<types::interface_t>()}) {
                check_impl_conformance(*rec, *iface);
            }
        }
    }

    last_type_.emplace(void_type);
}

auto type_resolver::check_impl_conformance(const impl_record& rec, const types::interface_t& iface)
    -> void {
    PROFILE_FUNCTION();
    // A parameterized impl may be conformance-checked while a different module is being
    // resolved (at the point its target first monomorphizes), so read AST from where it lives.
    const auto& cmod{rec.enclosing ? *rec.enclosing : resolving_};
    const auto  site_loc{cmod.ast.location_of(rec.site)};
    const auto  iface_name{ctx_.type_display_name(*rec.interface_type)};
    const auto  target_name{ctx_.type_display_name(*rec.target_type)};
    const auto& body_table{ctx_.registry.get(rec.body_scope_idx)};

    for (usize i{0}; i < iface.requirement_count; ++i) {
        const auto name{iface.method_names[i]};
        const auto supplied{rec.find_method(name)};
        if (!supplied) {
            ctx_.diags.emplace_back(fmt::format("`{}` does not implement `{}`: missing method `{}`",
                                                target_name,
                                                iface_name,
                                                name),
                                    error::MISSING_IMPL_METHOD,
                                    site_loc);
            continue;
        }
        const auto method_loc{cmod.ast.location_of(supplied->decl)};
        const auto expected{iface.method_sigs[i]->get_data().as_opt<types::function>()};
        const auto got{supplied->fn_type ? supplied->fn_type->get_data().as_opt<types::function>()
                                         : stdx::none};
        if (!expected || !got) { continue; }

        if (expected->has_self != got->has_self || expected->params.size() != got->params.size() ||
            expected->is_variadic != got->is_variadic) {
            ctx_.diags.emplace_back(
                fmt::format("method `{}` does not match the signature required by `{}` "
                            "(parameter count or `self` differs)",
                            name,
                            iface_name),
                error::IMPL_SIGNATURE_MISMATCH,
                method_loc);
            continue;
        }
        if (expected->has_self && !self_binding_compatible(*expected->params[0], *got->params[0])) {
            ctx_.diags.emplace_back(
                fmt::format(
                    "method `{}`: `self` binding is not compatible with the requirement in `{}`",
                    name,
                    iface_name),
                error::IMPL_SELF_MISMATCH,
                method_loc);
        }

        const usize first_param{expected->has_self ? 1UZ : 0UZ};
        for (usize p{first_param}; p < expected->params.size(); ++p) {
            auto& want{*expected->params[p]};
            auto& have{*got->params[p]};
            if (want.get_kind() == type_kind::TYPE) { continue; } // associated / Self slot
            if (!is_same_unqualified(want, have) && !is_assignable(have, want)) {
                ctx_.diags.emplace_back(
                    fmt::format(
                        "method `{}`: parameter {} type does not match the requirement in `{}`",
                        name,
                        p - first_param + 1,
                        iface_name),
                    error::IMPL_SIGNATURE_MISMATCH,
                    method_loc);
            }
        }

        if (expected->return_type.get_kind() != type_kind::TYPE &&
            !is_same_unqualified(expected->return_type, got->return_type)) {
            ctx_.diags.emplace_back(
                fmt::format("method `{}`: return type does not match the requirement in `{}`",
                            name,
                            iface_name),
                error::IMPL_SIGNATURE_MISMATCH,
                method_loc);
        }
    }

    // Every required associated type must be bound by the impl or defaulted by the interface.
    for (usize i{0}; i < iface.assoc_type_names.size(); ++i) {
        const bool defaulted{iface.ast_assoc_types[i].default_type.has_value()};
        if (!defaulted && !body_table.has(iface.assoc_type_names[i])) {
            ctx_.diags.emplace_back(
                fmt::format("`{}` does not implement `{}`: associated type `{}` is not bound",
                            target_name,
                            iface_name,
                            iface.assoc_type_names[i]),
                error::MISSING_IMPL_METHOD,
                site_loc);
        }
    }

    // A  static `var` is allowed for 'global' state but other members are not allowed
    for (const auto& entry : body_table) {
        const auto member_name{entry.first};

        // A parameterized `impl(P) ...` keeps its type params in this same scope
        const auto node{entry.second.symbol.get_data().as_opt<symbols::node_t>()};
        if (!node) { continue; }

        bool is_item{false};
        is_item |= std::ranges::contains(iface.method_names, member_name);
        is_item |= std::ranges::contains(iface.assoc_type_names, member_name);
        is_item |= std::ranges::contains(iface.assoc_const_names, member_name);

        const auto decl{cmod.ast.get_as_opt<ast::decl_stmt>(*node)};
        if (decl && decl->has_modifier(ast::decl_modifiers::VARIABLE)) { is_item = true; }

        if (!is_item) {
            ctx_.diags.emplace_back(
                fmt::format(
                    "`{}` is not a member of interface `{}`; put unrelated items in an inherent "
                    "`impl {}` block",
                    member_name,
                    iface_name,
                    target_name),
                error::UNKNOWN_IMPL_MEMBER,
                site_loc);
        }
    }
}

auto type_resolver::visit(ast::node_id id, const ast::using_stmt& using_stmt) -> void {
    PROFILE_FUNCTION();
    const auto& ident{resolving_.ast.get_as<ast::identifier_expr>(using_stmt.alias)};
    auto        sym{ctx_.registry.get_from_opt(table_idx_, ident.name)};
    if (!sym) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }
    if (sym->get_status() == symbol_status::RESOLVED) {
        auto& void_type{ctx_.get_builtin_resolved_type(type_kind::VOID_)};
        if (!resolving_.has_sema_type(id)) {
            resolving_.set_sema_type(using_stmt.alias, void_type);
            resolving_.set_sema_type(id, void_type);
        }
        return last_type_.emplace(void_type);
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
    auto* explicit_type_p{last_type_.take()};
    // `using X = MakesAType()`
    if (explicit_type_p->get_data().is<types::deferred_call>()) {
        const auto&     dc_call{explicit_type_p->get_data().as<types::deferred_call>().call};
        gir::const_eval evaluator{ctx_, resolving_};
        explicit_type_p = &evaluator.force_deferred_call(*explicit_type_p);
        register_non_generic_type_ctor_members(*explicit_type_p, dc_call);
    }
    auto& explicit_type{*explicit_type_p};
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
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_));
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
    if (fn.variadic) { fn_key.imprint(fn.variadic); }

    auto& resolved_fn{*ctx_.pool[fn_key]};
    resolved_fn.resolve_if<types::function>(param_types, return_type, false, fn.variadic);

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
        last_type_.emplace(ctx_.get_slice(
            array_element_mutability(array.mut_elements), null_terminated, item_type));
    }

    auto& final_type{apply_explicit_modifiers(id, *last_type_.take())};
    resolving_.set_sema_type(id, final_type);
    last_type_.emplace(final_type);
}

auto type_resolver::instantiate_generic(type&                             callee_type,
                                        const generic_function_info&      fn_info,
                                        gsl::span<type*>                  concrete_args,
                                        gsl::span<const gir::const_value> constexpr_args)
    -> stdx::option<generic_instantiation_entry> {
    mod::module& fn_mod{*fn_info.module};
    const auto&  fn_expr{*fn_info.fn_expr};
    const auto   fn_type{fn_info.fn_type};
    const auto   fn_table_idx{fn_type->get_symbol_table_idx()};

    // Snapshot the shared side tables so this typing can be captured as a diff and replayed
    const auto snap_nodes{fn_mod.sema_side_tables.node_types.values};
    const auto snap_types{fn_mod.sema_side_tables.explicit_types.values};
    const auto snap_ifs{fn_mod.if_constexpr_results};
    const auto snap_matches{fn_mod.match_arm_results};

    // Bind each `constexpr` parameter to its folded value while this instantiation's body is
    // resolved, so `const_eval` folds it there. `constexpr_args` is in parameter order.
    constexpr_frame binding_frame;
    for (usize p_idx{0}, cx_i{0}; p_idx < fn_expr.parameters.size(); ++p_idx) {
        if (!fn_expr.parameters[p_idx].is_constexpr) { continue; }
        if (cx_i >= constexpr_args.size()) { break; }
        const auto& name{
            fn_mod.ast.get_as<ast::identifier_expr>(fn_expr.parameters[p_idx].name).name};
        binding_frame.insert_or_assign(name, constexpr_args[cx_i]);
        ++cx_i;
    }
    const constexpr_frame_guard cfg{ctx_.constexpr_binding_frames, std::move(binding_frame)};

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
    inst_resolver.for_generic_instantiation_ = true;
    // This freestanding resolver has no enclosing-type context, so @this() needs it restored.
    stdx::option<structural_guard> this_type_guard;
    if (fn_info.enclosing_type) {
        this_type_guard.emplace(inst_resolver.user_type_stack_, *fn_info.enclosing_type);
    }
    // `constexpr` parameters are erased from the monomorph's signature.
    const auto      rt_param_count{static_cast<usize>(
        std::ranges::count_if(fn_expr.parameters, [](const auto& p) { return !p.is_constexpr; }))};
    auto            inst_param_types{ctx_.pool.get_many_unsafe(rt_param_count)};
    constexpr_frame type_param_frame;
    for (usize i{0};
         const auto& [arg_type, param] : std::views::zip(concrete_args, fn_expr.parameters)) {
        inst_resolver.resolve(param.explicit_type);
        type* decl_p_type{arg_type}; // erased type data corresponding to nominal signature type
        type* body_p_type{arg_type}; // contextual type meaning in the function body
        if (inst_resolver.last_type_ && !inst_resolver.last_type_->is_poison()) {
            auto& resolved_param_type{denoted_type(*inst_resolver.last_type_.take())};
            // `&auto` / `^auto` (from `impl I` sugar) has no concrete shape yet
            const auto strips_to_auto{[](auto&& self, const type& t) -> bool {
                if (t.get_kind() == type_kind::AUTO) { return true; }
                if (const auto p{t.get_data().as_opt<types::pointer>()}) {
                    return self(self, p->underlying);
                }
                if (const auto r{t.get_data().as_opt<types::reference>()}) {
                    return self(self, r->underlying);
                }
                return false;
            }};
            if (!strips_to_auto(strips_to_auto, resolved_param_type)) {
                decl_p_type = &resolved_param_type;
                if (resolved_param_type.get_kind() != type_kind::TYPE) {
                    body_p_type = &resolved_param_type;
                }
            }

            // A closure argument is never structurally a plain `fn(...)` value
            if (resolved_param_type.get_kind() == type_kind::FUNCTION &&
                arg_type->get_kind() == type_kind::CLOSURE) {
                const auto cl{arg_type->get_data().as_opt<types::closure_t>()};
                if (cl && is_same_unqualified(resolved_param_type, cl->signature)) {
                    decl_p_type = arg_type;
                    body_p_type = arg_type;
                } else {
                    const auto& expected{resolved_param_type.get_data().as<types::function>()};
                    ctx_.diags.emplace_back(
                        fmt::format("Closure does not match the parameter's expected call "
                                    "signature: expected {} parameter(s){}, arity/types differ",
                                    expected.params.size(),
                                    expected.is_variadic ? " (variadic)" : ""),
                        error::CLOSURE_SIGNATURE_MISMATCH,
                        fn_mod.ast.location_of(param.explicit_type));
                    return stdx::none;
                }
            }
            fn_mod.set_sema_type(param.explicit_type, resolved_param_type);
            if (resolved_param_type.get_kind() == type_kind::TYPE) {
                const auto& p_name{fn_mod.ast.get_as<ast::identifier_expr>(param.name).name};
                type_param_frame.insert_or_assign(p_name,
                                                  gir::const_value{denoted_type(*body_p_type)});
            }
        } else {
            fn_mod.set_sema_type(param.explicit_type, *decl_p_type);
        }
        fn_mod.set_sema_type(param.name, *body_p_type);
        if (!param.is_constexpr) { inst_param_types[i++] = decl_p_type; }
    }
    const constexpr_frame_guard type_param_guard{ctx_.constexpr_binding_frames,
                                                 std::move(type_param_frame)};
    inst_resolver.resolve(fn_expr.explicit_return_type);
    if (inst_resolver.last_type_->is_poison()) { return stdx::none; }
    // `denoted_type` unwraps a `@typeOf(param)` return annotation to the type it names, so it
    // isn't mistaken for a `fn(...): type` type constructor.
    auto&      return_type{denoted_type(*inst_resolver.last_type_.take())};
    const auto is_auto_return{return_type.get_kind() == type_kind::AUTO};
    inst_resolver.return_trackers_.emplace_back(return_tracker{
        .return_types   = {},
        .is_auto_return = is_auto_return,
        .expected_type  = is_auto_return ? stdx::none : stdx::option<type&>{return_type},
    });

    const auto  diags_before{ctx_.diags.size()};
    const auto& block{fn_mod.ast.get_as<ast::block_stmt>(fn_expr.body)};

    bool resolved_poison{false};
    {
        PROFILE_SCOPE("instantiate_generic: resolve body");
        for (const auto& stmt : block) {
            inst_resolver.resolve(stmt);
            if (inst_resolver.last_type_->is_poison()) { resolved_poison = true; }
        }
    }
    if (ctx_.diags.size() > diags_before || resolved_poison) {
        // Attribute diags here to `fn_mod`so they print against the defining file
        if (ctx_.diags.size() > diags_before && &fn_mod != &resolving_ &&
            !fn_mod.diagnostics.is<sema::diagnostics>()) {
            fn_mod.error_out(ctx_.diags.split_off(diags_before),
                             mod::module_state::POISONED_TYPE_RESOLVED);
        }
        return stdx::none;
    }

    // Diff the side tables against the snapshot: everything this instantiation's body/signature
    // resolved, to be replayed at emit time.
    body_type_diff typing;
    const auto     collect{[](const auto& live, const auto& snap, auto& out) {
        for (usize i{0}; i < live.size(); ++i) {
            if (!live[i]) { continue; }
            const bool changed{i >= snap.size() || !snap[i] || snap[i].get() != live[i].get()};
            if (changed) { out.emplace_back(i, live[i]); }
        }
    }};
    collect(fn_mod.sema_side_tables.node_types.values, snap_nodes, typing.node_types);
    collect(fn_mod.sema_side_tables.explicit_types.values, snap_types, typing.explicit_types);
    for (const auto& [idx, br] : fn_mod.if_constexpr_results) {
        const auto prev{snap_ifs.find(idx)};
        if (prev == snap_ifs.end() || prev->second != br) {
            typing.if_branches.emplace_back(idx, br);
        }
    }
    for (const auto& [idx, arm] : fn_mod.match_arm_results) {
        const auto prev{snap_matches.find(idx)};
        if (prev == snap_matches.end() || prev->second != arm) {
            typing.match_arms.emplace_back(idx, arm);
        }
    }

    auto tracker{std::move(inst_resolver.return_trackers_.back())};
    inst_resolver.return_trackers_.pop_back();

    auto mangled_name =
        fmt::format("{}__{}",
                    fn_info.name.value_or("fn"),
                    fmt::join(concrete_args | std::views::transform([&](const auto& arg) {
                                  return mangle_arg_type(ctx_.generic_functions, *arg);
                              }),
                              "_"));
    // Distinct `constexpr` argument values must produce distinct symbols.
    for (const auto& cx : constexpr_args) { mangled_name += fmt::format("_cx{}", cx.mangle()); }

    // A `fn(...): type` generic is a type constructor: every parameter is a `type` or a
    // `constexpr` value (erased from `inst_param_types`), and the body returns the type it builds.
    const bool all_params_are_types{std::ranges::all_of(
        inst_param_types, [](const type* p) { return p->get_kind() == type_kind::TYPE; })};
    const bool is_type_ctor{!is_auto_return && return_type.get_kind() == type_kind::TYPE &&
                            tracker.has_returns() && all_params_are_types};

    // Force constexpr param if constructing a type (inspo from zig)
    if (!is_type_ctor && !is_auto_return && return_type.get_kind() == type_kind::TYPE &&
        tracker.has_returns() && !all_params_are_types) {
        for (const auto& param : fn_expr.parameters) {
            if (param.is_constexpr) { continue; }
            const auto pty{fn_mod.get_sema_type_opt(param.explicit_type)};
            if (pty && pty->get_kind() != type_kind::TYPE) {
                const auto& pn{fn_mod.ast.get_as<ast::identifier_expr>(param.name).name};
                ctx_.diags.emplace_back(
                    fmt::format(
                        "a `fn(...): type` constructor cannot take a plain value parameter; "
                        "mark '{}' `constexpr` so it is known at instantiation time",
                        pn),
                    error::TYPE_MISMATCH,
                    fn_mod.ast.location_of(param.name));
                return stdx::none;
            }
        }
    }
    type* deduced_return_type{(is_auto_return || is_type_ctor) ? &tracker.deduced_return_type(ctx_)
                                                               : &return_type};

    // Copy into a per-instantiation type so `T(i32)` and `T(u8)` don't alias each other
    if (is_type_ctor) {
        const auto k{deduced_return_type->get_kind()};
        if (k == type_kind::STRUCT || k == type_kind::UNION || k == type_kind::ENUM) {
            if (const auto agg_node{returned_aggregate_node(fn_mod.ast, block)}) {
                auto& src_agg{*deduced_return_type};
                auto& clone{clone_anonymous_aggregate(
                    ctx_, src_agg, fmt::format("{}#{}", mangled_name, agg_node->get_index()))};
                register_type_ctor_members(
                    ctx_, fn_mod, *agg_node, src_agg, clone, mangled_name, std::move(typing));

                // Hand this instantiation's `constexpr` parameter values to the member emit
                std::vector<std::pair<std::string, gir::const_value>> ctor_bindings;
                for (usize p_idx{0}, cx_i{0}; p_idx < fn_expr.parameters.size(); ++p_idx) {
                    if (!fn_expr.parameters[p_idx].is_constexpr) { continue; }
                    if (cx_i >= constexpr_args.size()) { break; }
                    const auto& p_name{
                        fn_mod.ast.get_as<ast::identifier_expr>(fn_expr.parameters[p_idx].name)
                            .name};
                    ctor_bindings.emplace_back(std::string{p_name}, constexpr_args[cx_i]);
                    ++cx_i;
                }
                deduced_return_type = &clone;

                // Expand any parameterized `impl(P) [I for] Ctor(P)` onto this instantiation
                // (before `ctor_bindings` is moved -- the impl needs the ctor's folded values).
                if (!ctx_.impls.param_records().empty()) {
                    instantiate_impls_for(clone,
                                          fn_mod,
                                          fn_info.node_id,
                                          concrete_args,
                                          ctor_bindings,
                                          fn_expr,
                                          mangled_name);
                }

                if (!ctor_bindings.empty()) {
                    ctx_.instantiation_cache.set_type_ctor_bindings(mangled_name,
                                                                    std::move(ctor_bindings));
                }
            }
        }
    }

    // A type constructor has no runtime body
    if (!is_type_ctor) {
        // Hand the folded values to the emitter out-of-band
        if (!constexpr_args.empty()) {
            ctx_.instantiation_cache.set_constexpr_args(
                mangled_name,
                std::vector<gir::const_value>(constexpr_args.begin(), constexpr_args.end()));
        }
        if (!typing.empty()) {
            ctx_.instantiation_cache.set_body_type_diff(mangled_name, std::move(typing));
        }

        generic_instantiation_request req{
            .generic_fn_type = &callee_type,
            .arg_types       = inst_param_types,
            .return_type     = deduced_return_type,
            .mangled_name    = mangled_name,
            .fn_node_id      = fn_info.node_id,
            .module          = &fn_mod,
        };

        fn_mod.generic_instantiations.emplace_back(req);
        if (&resolving_ != &fn_mod) { resolving_.generic_instantiations.emplace_back(req); }
    }

    return generic_instantiation_entry{
        .return_type  = deduced_return_type,
        .mangled_name = std::move(mangled_name),
    };
}

} // namespace ghoti::sema
