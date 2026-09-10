#include "compiler/sema/unwrap_shape.hh"

#include <string>
#include <string_view>

#include <fmt/format.h>
#include <gsl/pointers>
#include <stdx/option.hh>

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

[[nodiscard]] auto find_assoc_type_alias(context&           ctx,
                                         const impl_record& rec,
                                         std::string_view   name) -> stdx::option<const type&> {
    if (const auto bsym{ctx.registry.get_from_opt(rec.body_scope_idx, name)}) {
        if (const auto bnode{bsym->get_data().as_opt<symbols::node_t>()}) {
            const auto& mod{rec.enclosing ? *rec.enclosing : ctx.modules.builtin_module()};
            if (const auto bu{mod.ast.get_as_opt<ast::using_stmt>(*bnode)}) {
                if (const auto t{mod.get_sema_type_opt(bu->explicit_type)}) {
                    if (t->is_resolved() && !t->is_poison()) { return denoted_type(*t); }
                }
            } else if (const auto bd{mod.ast.get_as_opt<ast::decl_stmt>(*bnode)}; bd && bd->value) {
                if (const auto t{mod.get_sema_type_opt(*bd->value)}) {
                    if (t->is_resolved() && !t->is_poison()) { return denoted_type(*t); }
                }
            }
        }
    }
    return stdx::none;
}

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

auto unwrap_shape_of(context& ctx, const type& operand) -> stdx::option<unwrap_info> {
    if (!ctx.prelude_index) { return stdx::none; }

    const auto& base{unref_type(denoted_type(operand))};
    const auto& iface{denoted_type(ctx.get_builtin_type("Unwrappable"))};

    const auto rec_opt{lookup_impl(ctx.impls, base, iface)};
    if (!rec_opt) { return stdx::none; }
    const auto& rec{*rec_opt};

    stdx::option<const type&> output_type;
    if (const auto m{rec.find_method("intoOutput")}; m && m->fn_type) {
        if (const auto fd{m->fn_type->get_data().as_opt<types::function>()}) {
            output_type.emplace(fd->return_type);
        }
    }
    if (!output_type) { output_type = find_assoc_type_alias(ctx, rec, "Output"); }
    if (!output_type) { return stdx::none; }

    stdx::option<const type&> residual_type;
    if (const auto m{rec.find_method("intoResidual")}; m && m->fn_type) {
        if (const auto fd{m->fn_type->get_data().as_opt<types::function>()}) {
            residual_type.emplace(fd->return_type);
        }
    }
    if (!residual_type) { residual_type = find_assoc_type_alias(ctx, rec, "Residual"); }
    if (!residual_type) { return stdx::none; }

    const bool residual_is_void{residual_type->get_kind() == type_kind::VOID_};

    return unwrap_info{
        .operand_type     = &base,
        .output_type      = output_type.get(),
        .residual_type    = residual_type.get(),
        .residual_is_void = residual_is_void,
        .impl             = &rec,
    };
}

auto rewrap_shape_of(context& ctx, const type& return_type) -> stdx::option<rewrap_info> {
    if (!ctx.prelude_index) { return stdx::none; }

    const auto& base{unref_type(denoted_type(return_type))};
    const auto& iface{denoted_type(ctx.get_builtin_type("Rewrappable"))};

    const auto rec_opt{lookup_impl(ctx.impls, base, iface)};
    if (!rec_opt) { return stdx::none; }
    const auto& rec{*rec_opt};

    stdx::option<const type&> from_type;
    if (const auto m{rec.find_method("fromResidual")}; m && m->fn_type) {
        if (const auto fd{m->fn_type->get_data().as_opt<types::function>()}) {
            if (!fd->params.empty()) { from_type.emplace(*fd->params[0]); }
        }
    }
    if (!from_type) { from_type = find_assoc_type_alias(ctx, rec, "From"); }
    if (!from_type) { return stdx::none; }

    return rewrap_info{
        .return_type = &base,
        .from_type   = from_type.get(),
        .impl        = &rec,
    };
}

} // namespace ghoti::sema
