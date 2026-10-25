#include <algorithm>
#include <ranges>
#include <string>
#include <utility>

#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "compiler/ast/format.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/type.hh"
#include "compiler/gir/const_eval.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/passes/type_resolver.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/keywords.hh"
#include "compiler/syntax/token_type.hh"
#include "internal.hh"
#include "support/int128.hh"

namespace ghoti::sema {

using namespace resolver_detail;

// Without a modifier or with poison the result should be the same as the node
auto type_resolver::apply_explicit_modifiers(ast::explicit_type_id id, type& inner_in) -> type& {
    const auto modifier{id.get_modifier()};
    if (modifier.is_value() || inner_in.is_poison()) { return inner_in; }

    // `^@TypeOf(x)` points at the type `@TypeOf(x)` denotes, never at a `type` value
    auto&      inner_type{denoted_type(inner_in)};
    const auto mutability{types::mut::from_type_modifier(modifier)};

    // Only a `dyn` view of an interface has a layout to point at; `&@This()` inside an interface
    // names the implementing type instead
    const auto names_this{[&] {
        const auto call{resolving_.ast.get_as_opt<ast::call_expr>(id)};
        return call && call->function->get_builtin() == syntax::builtin_t::THIS;
    }};
    if ((modifier.is_ptr() || modifier.is_ref()) && inner_type.get_kind() == type_kind::INTERFACE &&
        !names_this()) {
        const auto iname{ctx_.type_display_name(inner_type)};
        const auto sigil{fmt::format("{}{}",
                                     modifier.is_ptr() ? "^" : "&",
                                     mutability == types::mut::MUTABLE ? "mut " : "")};
        return ctx_.poison_node(
            resolving_,
            id,
            fmt::format("`{0}{1}` names an interface, not a type with a layout; use `{0}dyn {1}`",
                        sigil,
                        iname),
            error::INTERFACE_NOT_A_VALUE,
            resolving_.ast.location_of(id));
    }

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
        // A container's qualifier bits describe what it reaches, so `volatile` can't be its own
        const auto reject{[&](std::string message) -> type& {
            return ctx_.poison_node(resolving_,
                                    id,
                                    std::move(message),
                                    error::ILLEGAL_VOLATILE_QUALIFIER,
                                    resolving_.ast.location_of(id));
        }};
        const auto name{ctx_.type_display_name(inner_type)};
        switch (inner_type.get_kind()) {
        case type_kind::POINTER:
        case type_kind::REFERENCE:
        case type_kind::SLICE:
            return reject(fmt::format("`volatile` can't qualify '{}' itself; to make what it "
                                      "reaches volatile, put `volatile` after its `^`, `&`, or "
                                      "`[]` (`^volatile T`)",
                                      name));
        default: break;
        }
        // A sized array is still deferred until its length folds
        if (inner_type.get_kind() == type_kind::ARRAY ||
            inner_type.get_data().is<types::deferred_array>()) {
            return reject(fmt::format("`volatile` on an array qualifies its elements; write "
                                      "`[N]volatile T` instead of `volatile {}`",
                                      name));
        }
        if (inner_type.is_volatile()) {
            return reject(fmt::format("'{}' is already volatile", name));
        }

        // Volatility is baked into mutability and should not be imprinted
        auto& new_vol_type{*ctx_.pool[new_key]};
        new_vol_type.resolve_if<type::data_t>(inner_type.get_data());
        new_vol_type.copy_identity_from(inner_type);
        return new_vol_type;
    }
    UNREACHABLE("A new type modifier was likely added yet unaccounted for");
}

auto type_resolver::visit(ast::explicit_type_id id, const ast::identifier_expr& ident) -> void {
    PROFILE_FUNCTION();

    // A builtin names an operation, never a type
    if (id.get_token_type() == syntax::token_type_t::BUILTIN) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("Builtin '{}' is not a type", ident.name),
                             error::TYPE_MISMATCH,
                             resolving_.ast.location_of(id)));
    }

    // `iN` / `uN` resolve straight to a pooled integer type, no symbol lookup.
    if (syntax::token_type::is_int_type_lexeme(ident.name) &&
        (id.get_token_type() == syntax::token_type_t::INT_TYPE ||
         !ctx_.registry.lookup_with_depth(table_stack_, ident.name))) {
        resolve_ident(id, ident);
        auto& resolved{apply_explicit_modifiers(id, *last_type_.take())};
        resolving_.set_sema_type(id, resolved);
        return last_type_.emplace(resolved);
    }

    const auto            found{ctx_.registry.lookup_with_table(table_stack_, ident.name)};
    stdx::option<symbol&> symbol_opt;
    usize                 symbol_table{0};
    if (found) {
        symbol_opt.emplace(found->symbol);
        symbol_table = found->table_idx;
    }
    // A primitive keyword names the primitive even where a raw identifier declares its name
    if (std::ranges::contains(syntax::ALL_PRIMITIVES, id.get_token_type())) {
        if (const auto primitive{ctx_.registry.get(*ctx_.prelude_index).get_opt(ident.name)}) {
            symbol_opt.emplace(*primitive);
            symbol_table = *ctx_.prelude_index;
        }
    }
    bool overrode_shadow{false};
    if (symbol_opt &&
        (!symbol_opt->has_kind() || symbol_opt->get_kind() != sema::symbol_kind::TYPE)) {
        for (const auto idx : table_stack_ | std::views::reverse) {
            if (const auto sym{ctx_.registry.get(idx).get_opt(ident.name)};
                sym && sym->has_kind() && sym->get_kind() == sema::symbol_kind::TYPE) {
                symbol_opt.emplace(*sym);
                symbol_table    = idx;
                overrode_shadow = true;
                break;
            }
        }
    }
    if (!symbol_opt) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("Use of undeclared identifier '{}'", ident.name),
                             error::UNDECLARED_IDENTIFIER,
                             resolving_.ast.location_of(id)));
    }
    auto& sym{*symbol_opt};
    resolving_.set_identifier_definition(id, {resolving_.path, sym.get_symbol_span(resolving_)});
    record_scoped_declaration(id, symbol_table, sym);

    // A `comptime let mut` type names whatever type it holds at this point
    if (sym.get_status() == symbol_status::RESOLVED) {
        if (const auto current{comptime_type_var_value(sym)}) {
            auto& resolved{apply_explicit_modifiers(id, *current)};
            resolving_.set_sema_type(id, resolved);
            return last_type_.emplace(resolved);
        }
    }

    // `resolve_ident` re-looks-up the name from scratch, which would just rediscover the
    // shadowing symbol we already routed around above. Resolve the override directly instead.
    if (overrode_shadow) {
        resolve_symbol(id, sym);
    } else {
        const auto forwarded_type{
            forward_type(resolving_, id.get_modifier(), sym, resolving_dyn_referent_)};
        const auto mods{id.get_modifier()};
        // A by-value reference to an aggregate whose own resolution has not started yet
        if (forwarded_type && !forwarded_type->is_resolved() &&
            sym.get_status() == symbol_status::UNRESOLVED &&
            (mods.is_value() || (!mods.is_ptr() && !mods.is_ref()))) {
            resolve_ident(id, ident);
        } else {
            forwarded_type ? last_type_.emplace(*forwarded_type) : resolve_ident(id, ident);
        }
    }

    if (!last_type_->is_poison() && names_a_value(sym, symbol_table)) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("'{}' is a value, not a type", ident.name),
                             error::TYPE_MISMATCH,
                             resolving_.ast.location_of(id)));
    }

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

MAKE_MODIFIED_RESOLVER(dot_expr, resolve_dot)

MAKE_MODIFIED_RESOLVER(call_expr, resolve_call)

auto type_resolver::visit(ast::explicit_type_id id, const ast::explicit_function_type& fn) -> void {
    PROFILE_FUNCTION();
    // A function type's own parameters aren't patterns to deduce from, unless it's an `impl Fn`
    const mutating_context_guard not_param{in_param_type_, fn.is_impl_fn};
    auto                         param_types{ctx_.pool.get_many_unsafe(fn.parameter_types.size())};

    if (!fn.is_extern && fn.has_explicit_conv) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_, id, callconv_requires_extern(resolving_.ast.location_of(id))));
    }

    // A type-building call (`@Vector(4, f32)`) names the type it builds, not a `type` value
    for (usize i{0}; const auto& param : fn.parameter_types) {
        TRY_RESOLVE(param);
        auto& param_type{denoted_type(*last_type_.take())};
        if (param_type.get_kind() == type_kind::AUTO && !fn.is_impl_fn) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 "Function types cannot have 'auto' parameter types",
                                 error::ILLEGAL_AUTO_USAGE,
                                 resolving_.ast.location_of(param)));
        }
        if (param_type.get_kind() == type_kind::OPAQUE) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_, id, opaque_by_value_error(resolving_.ast.location_of(param))));
        }
        param_types[i++] = &param_type;
    }

    TRY_RESOLVE(fn.explicit_return_type);
    auto& return_type{denoted_type(*last_type_.take())};
    if (return_type.get_kind() == type_kind::AUTO && !fn.is_impl_fn) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "Function types cannot have 'auto' return type",
                             error::ILLEGAL_AUTO_USAGE,
                             resolving_.ast.location_of(fn.explicit_return_type)));
    }
    if (return_type.get_kind() == type_kind::OPAQUE) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            opaque_by_value_error(resolving_.ast.location_of(fn.explicit_return_type))));
    }
    auto& resolved_fn{
        ctx_.get_function(param_types, return_type, fn.variadic, fn.conv, !fn.is_extern)};
    if (fn.is_extern) {
        if (auto problem{unmodeled_c_abi_error(resolved_fn, resolving_.ast.location_of(id))}) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(*problem)));
        }
    }

    // `&fn(...)` borrows the same non-null callable value that a bare `fn(...)` already is
    auto& final_type{id.get_modifier().is_ref() ? resolved_fn
                                                : apply_explicit_modifiers(id, resolved_fn)};
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

// A dimension that already folds must be a non-negative count of an addressable number of bytes
auto type_resolver::check_array_dimension(ast::expr_handle dimension, const type& item_type)
    -> stdx::option<diagnostic> {
    const auto dim_type{resolving_.get_sema_type_opt(dimension)};
    if (dim_type && !is_integer(dim_type->get_kind()) &&
        dim_type->get_kind() != type_kind::COMPTIME_INT && !is_generic_type(*dim_type)) {
        return diagnostic{fmt::format("Array length must be an integer; found '{}'",
                                      ctx_.type_display_name(*dim_type)),
                          error::TYPE_MISMATCH,
                          resolving_.ast.location_of(dimension)};
    }

    gir::const_eval evaluator{ctx_, resolving_};
    const auto      folded{evaluator.try_eval(dimension)};
    const auto      len{folded ? folded->as_int_opt() : stdx::none};
    if (!len) { return stdx::none; }
    if (*len < 0) {
        return diagnostic{"Array length cannot be negative",
                          error::TYPE_MISMATCH,
                          resolving_.ast.location_of(dimension)};
    }

    // Past 2^48 bytes no target can address the array, and sizes start overflowing
    constexpr i128 max_array_bytes{i128{1} << 48};
    const auto     elem_size{has_fixed_bit_layout(item_type)
                                 ? gir::const_eval::type_size_of(item_type, target_layout())
                                 : usize{1}};
    if (*len > max_array_bytes || *len * static_cast<i128>(elem_size) > max_array_bytes) {
        return diagnostic{
            "Array type is too large", error::TYPE_MISMATCH, resolving_.ast.location_of(dimension)};
    }
    return stdx::none;
}

auto type_resolver::visit(ast::explicit_type_id id, const ast::explicit_array_type& array) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(array.inner_explicit_type);
    auto& item_type{denoted_type(*last_type_.take())};

    if (item_type.get_kind() == type_kind::AUTO && !in_param_type_) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "Array elements can only have type 'auto' in a function parameter",
                             error::ILLEGAL_AUTO_USAGE,
                             resolving_.ast.location_of(array.inner_explicit_type)));
    }
    if (item_type.get_kind() != type_kind::AUTO &&
        reject_storage_slot(array.inner_explicit_type, item_type)) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id));
    }

    // A reference is a borrow, not a storable slot: writing `[N]&T` / `[]&T` is illegal
    if (array.inner_explicit_type.get_modifier().is_ref()) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            "an array or slice element cannot be a reference; use a raw pointer (`^T`)",
            error::ILLEGAL_REFERENCE_FIELD,
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
        if (auto diag{check_array_dimension(*array.dimension, item_type)}) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(*diag)));
        }
        // Keyed on the element type too, so a generic's `[n]T` doesn't keep the first
        // instantiation's `T`
        last_type_.emplace(ctx_.pool[{type_kind::TYPE, types::mut::CONSTANT, &array, &item_type}]);
        last_type_->resolve_if<types::deferred_array>(array, item_type, resolving_);
    } else {
        last_type_.emplace(
            ctx_.get_slice(array_element_mutability(array.mut_elements, array.poly_elements),
                           null_terminated,
                           item_type));
    }

    auto& final_type{apply_explicit_modifiers(id, *last_type_.take())};
    resolving_.set_sema_type(id, final_type);
    last_type_.emplace(final_type);
}

auto type_resolver::visit(ast::explicit_type_id id, const ast::explicit_expr_type& node) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(node.expr);
    if (last_type_.take()->is_poison()) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id));
    }

    // The type it computes is known at compile time
    const auto                                    diags_before{ctx_.diags.size()};
    gir::const_eval                               evaluator{ctx_, resolving_};
    const gir::const_eval::comptime_context_guard g{evaluator, true};
    const auto                                    folded{evaluator.try_eval(node.expr)};
    if (ctx_.diags.size() > diags_before) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id));
    }
    const auto denoted{folded ? folded->as_opt<stdx::option<type&>>() : stdx::none};
    if (folded && !folded->is_poison() && (!denoted || !*denoted)) {
        return last_type_.emplace(ctx_.poison_node(resolving_,
                                                   id,
                                                   "This expression is a value, not a type",
                                                   error::TYPE_MISMATCH,
                                                   resolving_.ast.location_of(node.expr)));
    }
    if (!folded && (in_param_type_ || in_return_type_)) {
        // A signature's type can read a comptime parameter, which each instantiation binds
        auto& placeholder{ctx_.get_builtin_resolved_type(type_kind::TYPE)};
        resolving_.set_sema_type(id, placeholder);
        return last_type_.emplace(placeholder);
    }
    if (!denoted || !*denoted) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "A type computed by an expression must be known at compile time",
                             error::COMPTIME_EVALUATION_FAILED,
                             resolving_.ast.location_of(node.expr)));
    }

    auto& resolved{apply_explicit_modifiers(id, denoted_type(**denoted))};
    resolving_.set_sema_type(id, resolved);
    last_type_.emplace(resolved);
}

auto type_resolver::visit(ast::explicit_type_id id, const ast::explicit_dyn_type& dyn) -> void {
    PROFILE_FUNCTION();
    const bool unsized_ok{std::exchange(dyn_unsized_ok_, false)};
    {
        // A `dyn` view never needs its interface's layout, so it may name one still resolving.
        // By value it is rejected as unsized below.
        const mutating_context_guard referent{resolving_dyn_referent_, true};
        TRY_RESOLVE(dyn.interface_type);
    }
    auto& iface_type{denoted_type(*last_type_.take())};
    if (iface_type.get_kind() != type_kind::INTERFACE) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("`dyn` requires an interface; found `{}`",
                                         ctx_.type_display_name(iface_type)),
                             error::TYPE_MISMATCH,
                             resolving_.ast.location_of(dyn.interface_type)));
    }

    // An interface still resolving its own signatures binds against what it has resolved so far
    gsl::span<std::string_view>                      assoc_type_names;
    gsl::span<const ast::interface_expr::assoc_type> ast_assoc_types;
    gsl::not_null<const mod::module_t*>              enclosing{&resolving_};
    if (iface_type.is_resolved()) {
        const auto& iface{iface_type.get_data().as<types::interface_t>()};
        assoc_type_names = iface.assoc_type_names;
        ast_assoc_types  = iface.ast_assoc_types;
        enclosing        = &iface.enclosing;
    } else {
        const auto open{std::ranges::find(
            open_interfaces_, &iface_type, [](const auto& o) { return o.iface.get(); })};
        if (open == open_interfaces_.end()) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 fmt::format("'{}' is used during its own resolution",
                                             ctx_.type_display_name(iface_type)),
                                 error::CYCLIC_DEPENDENCY,
                                 resolving_.ast.location_of(dyn.interface_type)));
        }
        assoc_type_names = open->assoc_type_names;
        ast_assoc_types  = open->ast_assoc_types;
    }

    // Resolve the `(Assoc = T, ...)` list into per-associated-type slots in interface order.
    auto bindings{ctx_.pool.get_many_pending(assoc_type_names.size())};
    for (const auto& ab : dyn.assoc_bindings) {
        const auto& bname{resolving_.ast.get_as<ast::identifier_expr>(ab.name).name};
        const auto  slot{std::ranges::find(assoc_type_names, bname)};
        if (slot == assoc_type_names.end()) {
            ctx_.diags.emplace_back(fmt::format("`{}` has no associated type `{}`",
                                                ctx_.type_display_name(iface_type),
                                                bname),
                                    error::DYN_UNBOUND_ASSOC,
                                    resolving_.ast.location_of(ab.name));
            continue;
        }
        TRY_RESOLVE(ab.type);
        bindings[static_cast<usize>(slot - assoc_type_names.begin())] =
            denoted_type(*last_type_.take());
    }

    // `dyn`-safety: every associated type must be pinned here or defaulted. Fill defaulted slots
    // so `dyn_t.assoc_bindings` is complete for method-signature substitution.
    for (usize i{0}; i < assoc_type_names.size(); ++i) {
        if (bindings[i]) { continue; }
        if (!ast_assoc_types[i].default_type) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("`&dyn {}` needs associated type `{}` bound: write `dyn {}({} = ...)`",
                            ctx_.type_display_name(iface_type),
                            assoc_type_names[i],
                            ctx_.type_display_name(iface_type),
                            assoc_type_names[i]),
                error::DYN_UNBOUND_ASSOC,
                resolving_.ast.location_of(id)));
        }
        // The default's node lives in the interface's module, which already resolved it
        const auto default_type{enclosing->get_sema_type_opt(*ast_assoc_types[i].default_type)};
        if (!default_type || default_type->is_poison()) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id));
        }
        bindings[i] = denoted_type(*default_type);
    }

    // An open interface's methods aren't all known yet, so it is checked once it commits
    if (!iface_type.is_resolved()) {
        deferred_dyn_checks_.emplace_back(&iface_type, resolving_.ast.location_of(id));
    } else if (auto problem{dyn_safety_problem(iface_type)}) {
        return last_type_.emplace(ctx_.poison_node(resolving_,
                                                   id,
                                                   std::move(*problem),
                                                   error::DYN_BY_VALUE_SELF,
                                                   resolving_.ast.location_of(id)));
    }

    // Every slot is bound or defaulted by now
    auto& dyn_type{ctx_.get_dyn(iface_type, ctx_.pool.settle(bindings))};

    // `dyn I` is unsized: legal only as the referent of `&` / `^`.
    auto& final_type{apply_explicit_modifiers(id, dyn_type)};
    if (final_type.get_kind() == type_kind::DYN && !unsized_ok) {
        return last_type_.emplace(ctx_.poison_node(resolving_,
                                                   id,
                                                   "`dyn I` is unsized; use `&dyn I` or `^dyn I`",
                                                   error::ILLEGAL_UNSIZED_TYPE,
                                                   resolving_.ast.location_of(id)));
    }
    resolving_.set_sema_type(id, final_type);
    last_type_.emplace(final_type);
}

// A method must take `self` by `&`/`^` and must not mention `@This()` directly
auto type_resolver::dyn_safety_problem(const type& iface_type) const -> stdx::option<std::string> {
    const auto& iface{iface_type.get_data().as<types::interface_t>()};
    const auto  is_dyn_unsafe_slot{
        [](const type& t) { return t.get_kind() == type_kind::INTERFACE; }};
    for (usize i{0}; i < iface.method_names.size(); ++i) {
        const auto fn{iface.method_sigs[i]->get_data().as_opt<types::function>()};
        if (!fn) { continue; }
        bool unsafe{fn->has_self && !fn->params.empty() &&
                    fn->params[0]->get_kind() != type_kind::POINTER &&
                    fn->params[0]->get_kind() != type_kind::REFERENCE};
        for (usize p{fn->has_self ? 1UZ : 0UZ}; !unsafe && p < fn->params.size(); ++p) {
            unsafe = is_dyn_unsafe_slot(*fn->params[p]);
        }
        unsafe = unsafe || is_dyn_unsafe_slot(fn->return_type);
        if (unsafe) {
            return fmt::format("`{}` is not `dyn`-safe: method `{}` passes `self`, `@This()`, or "
                               "an unbound associated type by value",
                               ctx_.type_display_name(iface_type),
                               iface.method_names[i]);
        }
    }
    return stdx::none;
}

auto type_resolver::check_deferred_dyn_safety(const type& iface_type) -> void {
    const auto problem{dyn_safety_problem(iface_type)};
    std::erase_if(deferred_dyn_checks_, [&](const deferred_dyn_check& check) {
        if (check.iface.get() != &iface_type) { return false; }
        if (problem) {
            ctx_.diags.emplace_back(*problem, error::DYN_BY_VALUE_SELF, check.location);
        }
        return true;
    });
}

// An array whose length folds late is sized before it meets its peers, behind a pointer too
auto type_resolver::peer_view(type& t) -> type& {
    auto&      sized{concrete_array_type(t)};
    const auto mutability{sized.is_constant() ? types::mut::CONSTANT : types::mut::MUTABLE};
    if (const auto ptr{sized.get_data().as_opt<types::pointer>()}) {
        auto& pointee{concrete_array_type(ptr->underlying)};
        return &pointee == &ptr->underlying ? sized : ctx_.get_pointer(mutability, pointee);
    }
    if (const auto ref{sized.get_data().as_opt<types::reference>()}) {
        auto& referent{concrete_array_type(ref->underlying)};
        return &referent == &ref->underlying ? sized : ctx_.get_reference(mutability, referent);
    }
    return sized;
}

auto type_resolver::concrete_array_type(type& maybe_deferred) -> type& {
    if (!maybe_deferred.get_data().is<types::deferred_array>()) { return maybe_deferred; }
    gir::const_eval evaluator{ctx_, resolving_};
    return evaluator.force_deferred_array(maybe_deferred);
}

} // namespace ghoti::sema
