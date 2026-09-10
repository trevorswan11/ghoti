#include "compiler/sema/unwrap_shape.hh"

#include <string>
#include <string_view>

#include <fmt/format.h>
#include <gsl/pointers>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/ast.hh"
#include "compiler/ast/statement.hh"
#include "compiler/gir/symbol_scoping.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/impl_registry.hh"
#include "compiler/sema/type.hh"

namespace ghoti::sema {

namespace {

[[nodiscard]] auto denoted_type(const type& t) noexcept -> const type& {
    if (t.get_kind() == type_kind::TYPE) {
        if (const auto meta{t.get_data().as_opt<types::meta_type>()}) { return meta->instance; }
    }
    return t;
}

[[nodiscard]] auto unref_type(const type& t) noexcept -> const type& {
    if (const auto ref{t.get_data().as_opt<types::reference>()}) { return ref->underlying; }
    return t;
}

} // namespace

// Looks up an associated type alias (`using Output = ...` or `const Output := ...`)
// in the impl's body scope. If the impl is parameterized (`from_parameterized`), any
// sentinel types are remapped to concrete type arguments.
auto find_assoc_type_alias(context& ctx, const impl_record& rec, std::string_view name)
    -> stdx::option<const type&> {
    if (const auto bsym{ctx.registry.get_from_opt(rec.body_scope_idx, name)}) {
        if (const auto bnode{bsym->get_data().as_opt<symbols::node_t>()}) {
            const auto& mod{rec.enclosing ? *rec.enclosing : ctx.modules.builtin_module()};
            stdx::option<const type&> result;
            if (const auto bu{mod.ast.get_as_opt<ast::using_stmt>(*bnode)}) {
                if (const auto t{mod.get_sema_type_opt(bu->explicit_type)}) {
                    if (t->is_resolved() && !t->is_poison()) { result.emplace(denoted_type(*t)); }
                }
            } else if (const auto bd{mod.ast.get_as_opt<ast::decl_stmt>(*bnode)}; bd && bd->value) {
                if (const auto t{mod.get_sema_type_opt(*bd->value)}) {
                    if (t->is_resolved() && !t->is_poison()) { result.emplace(denoted_type(*t)); }
                }
            }
            if (result) {
                // For parameterized impls (e.g. `impl(T, E) Unwrappable for Result(T, E)`),
                // replace sentinels with the actual concrete type arguments instantiated for this
                // target.
                if (rec.from_parameterized) {
                    stdx::option<type&> r{const_cast<type&>(*result)};
                    for (usize i{0}; i < rec.sentinels.size() && i < rec.type_arguments.size();
                         ++i) {
                        r.emplace(remap_type(
                            ctx, *r, *rec.sentinels[i], *const_cast<type*>(rec.type_arguments[i])));
                    }
                    return *r;
                }
                return *result;
            }
        }
    }
    return stdx::none;
}

namespace {

[[nodiscard]] auto lookup_impl(const impl_registry& registry, const type& target, const type& iface)
    -> stdx::option<const impl_record&> {
    if (const auto exact{registry.lookup(target, iface)}) { return *exact; }
    for (const auto* r : registry.records()) {
        if (r->target_type && is_same_unqualified(*r->target_type, target) && r->interface_type &&
            is_same_unqualified(*r->interface_type, iface)) {
            return *r;
        }
    }
    return stdx::none;
}

} // namespace

auto unwrap_info::gir_method_name(std::string_view           method_name,
                                  const gir::symbol_scoping& scoping) const -> std::string {
    if (impl->from_parameterized) { return fmt::format("{}.{}", impl->gir_prefix, method_name); }
    return scoping.name_for(impl->body_scope_idx, method_name);
}

auto rewrap_info::gir_method_name(std::string_view           method_name,
                                  const gir::symbol_scoping& scoping) const -> std::string {
    if (impl->from_parameterized) { return fmt::format("{}.{}", impl->gir_prefix, method_name); }
    return scoping.name_for(impl->body_scope_idx, method_name);
}

// Deep structural substitution: replaces all occurrences of `from` with `to` across
// pointer/reference/slice/array wrappers and function parameter/return types.
// Preserves type pool identity when no sub-type changes.
auto remap_type(context& ctx, type& t, const type& from, type& to) -> type& {
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

// Queries whether `operand` implements `builtin.Unwrappable`. If so, extracts the
// associated `Output` and `Residual` types and the `Flow(Output, Residual)` return
// type of `branch(self)`.
auto unwrap_shape_of(context& ctx, const type& operand) -> stdx::option<unwrap_info> {
    if (!ctx.prelude_index) { return stdx::none; }

    const auto& base{unref_type(denoted_type(operand))};
    const auto& iface{denoted_type(ctx.get_builtin_type(builtin_impl::UNWRAPPABLE))};

    const auto rec_opt{lookup_impl(ctx.impls, base, iface)};
    if (!rec_opt) { return stdx::none; }
    const auto& rec{*rec_opt};

    stdx::option<const type&> output_type{find_assoc_type_alias(ctx, rec, builtin_impl::OUTPUT)};
    stdx::option<const type&> residual_type{
        find_assoc_type_alias(ctx, rec, builtin_impl::RESIDUAL)};
    stdx::option<const type&> flow_type;

    // Retrieve concrete `Flow(...)` return type from `branch(self)`
    if (const auto m{rec.find_method(builtin_impl::BRANCH)}; m && m->fn_type) {
        if (const auto fd{m->fn_type->get_data().as_opt<types::function>()}) {
            flow_type.emplace(fd->return_type);
        }
    }

    if (!output_type || !residual_type) { return stdx::none; }

    const bool residual_is_void{residual_type->get_kind() == type_kind::VOID_};

    return unwrap_info{
        .operand_type     = &base,
        .output_type      = output_type.get(),
        .residual_type    = residual_type.get(),
        .flow_type        = flow_type,
        .residual_is_void = residual_is_void,
        .impl             = &rec,
    };
}

auto rewrap_shape_of(context& ctx, const type& return_type) -> stdx::option<rewrap_info> {
    if (!ctx.prelude_index) { return stdx::none; }

    const auto& base{unref_type(denoted_type(return_type))};
    const auto& iface{denoted_type(ctx.get_builtin_type(builtin_impl::REWRAPPABLE))};

    const auto rec_opt{lookup_impl(ctx.impls, base, iface)};
    if (!rec_opt) { return stdx::none; }
    const auto& rec{*rec_opt};

    stdx::option<const type&> from_type;
    if (const auto m{rec.find_method(builtin_impl::FROM_RESIDUAL)}; m && m->fn_type) {
        if (const auto fd{m->fn_type->get_data().as_opt<types::function>()}) {
            if (!fd->params.empty()) { from_type.emplace(*fd->params[0]); }
        }
    }
    if (!from_type) { from_type = find_assoc_type_alias(ctx, rec, builtin_impl::FROM); }
    if (!from_type) { return stdx::none; }

    return rewrap_info{
        .return_type = &base,
        .from_type   = from_type.get(),
        .impl        = &rec,
    };
}

} // namespace ghoti::sema
