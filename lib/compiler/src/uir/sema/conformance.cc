#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/expression.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"

namespace ghoti::uir::sema {

namespace {

using modifier = ast::type_modifier::modifier;

} // namespace

auto shape_of_signature(const module_t& m, inst_id func) -> self_shape {
    record_reader rec{m, m.data_of(func).pl.extra};
    const auto    params{params_of(m, rec.body_field())};
    self_shape    shape{.count = params.size()};
    if (params.empty()) { return shape; }

    record_reader p{m, m.data_of(params.front()).pl.extra};
    p.skip_str().skip_body().skip_body();
    const auto param_flags{p.u32_field()};
    const auto how{static_cast<modifier>(p.u32_field())};
    if ((param_flags & flags::PARAM_SELF) != 0) {
        shape.has_self = true;
        shape.how      = how;
    }
    return shape;
}

auto associated_items(const module_t& m, inst_id iface, bool types)
    -> std::vector<associated_item> {
    record_reader rec{m, m.data_of(iface).pl.extra};
    for (u32 i{0}, count{rec.count()}; i < count; ++i) { rec.skip_ref().skip_u32().skip_str(); }
    rec.skip_body();

    std::vector<associated_item> items;
    for (u32 list{0}; list < 2; ++list) {
        for (u32 i{0}, count{rec.count()}; i < count; ++i) {
            const auto name{rec.str_field()};
            rec.skip_body();
            const bool defaulted{!rec.body_field().empty()};
            if ((list == 0) == types) { items.emplace_back(associated_item{name, defaulted}); }
        }
    }
    return items;
}

namespace {

[[nodiscard]] auto is_bodyless(const module_t& m, inst_id func) -> bool {
    record_reader rec{m, m.data_of(func).pl.extra};
    rec.skip_body().skip_body().skip_body().skip_u32();
    return (rec.u32_field() & flags::BODYLESS) != 0;
}

// A `&mut` or `^mut` requirement needs a mutable binding, pointer and reference must agree, and
// only a `mut?` method implements a `mut?` one
[[nodiscard]] auto self_compatible(modifier required, modifier provided) -> bool {
    const auto is_poly{[](modifier h) { return h == modifier::POLY_REF || h == modifier::POLY_PTR; }};
    if (is_poly(required) || is_poly(provided)) { return required == provided; }
    switch (required) {
    case modifier::PTR:     return provided == modifier::PTR || provided == modifier::MUT_PTR;
    case modifier::MUT_PTR: return provided == modifier::MUT_PTR;
    case modifier::REF:     return provided == modifier::REF || provided == modifier::MUT_REF;
    case modifier::MUT_REF: return provided == modifier::MUT_REF;
    default:                return provided == modifier::VALUE;
    }
}

// The function a member's value is, through the closure over associated items it may be
[[nodiscard]] auto declared_func(const module_t& m, const decl& d) -> stdx::option<inst_id> {
    const auto body{m.body(d.value_body)};
    if (body.empty() || m.tag_of(body.back()) != tag::yield) { return stdx::none; }
    auto yielded{as_inst(m.data_of(body.back()).un.operand)};
    if (yielded && m.tag_of(*yielded) == tag::closure) {
        record_reader closure{m, m.data_of(*yielded).pl.extra};
        yielded = as_inst(*closure.ref_field());
    }
    if (!yielded || m.tag_of(*yielded) != tag::func) { return stdx::none; }
    return yielded;
}

} // namespace

auto compilation::check_impls(gs::type&                                       target,
                              gsl::span<const gsl::not_null<impl_instance*>> applied) -> void {
    // A check may apply the impls again, which it then has nothing left to do for
    if (!checked_impls_.insert(&target).second) { return; }
    auto& ctx{ctx_};

    // An impl belongs to the module that declares its interface or its target
    const auto home{[&](gs::type& t) -> const unit* {
        const auto n{nominal_of(t)};
        return n ? n->owner.get() : nullptr;
    }};
    for (const auto impl : applied) {
        const auto& u{*impl->entry->owner};
        const bool  target_here{home(*impl->target) == &u};
        const auto& at{u.uir->src_of(impl->entry->inst)};
        if (impl->interface && !target_here && home(*impl->interface) != &u) {
            DISCARD(report(u,
                           at,
                           gs::error::ORPHAN_IMPL,
                           "`impl I for T` is only allowed in the module that declares `I` or `T`"));
        } else if (!impl->interface && !target_here) {
            DISCARD(report(u,
                           at,
                           gs::error::ORPHAN_IMPL,
                           "an inherent `impl T` requires `T` to be declared in this module"));
        }
    }

    // An interface is implemented once per type
    for (usize i{0}; i < applied.size(); ++i) {
        const auto& later{*applied[i]};
        if (!later.interface) { continue; }
        for (usize j{0}; j < i; ++j) {
            if (!applied[j]->interface || &*applied[j]->interface != &*later.interface) {
                continue;
            }
            const auto& u{*later.entry->owner};
            DISCARD(report(u,
                           u.uir->src_of(later.entry->inst),
                           gs::error::DUPLICATE_IMPL,
                           fmt::format("`{}` already implements `{}`",
                                       ctx.type_display_name(target),
                                       ctx.type_display_name(*later.interface))));
            break;
        }
    }

    // An inherent impl adds members of its own, which no other member may share
    std::vector<std::string_view> names;
    if (const auto n{nominal_of(target)}) {
        if (const auto ns{n->owner->uir->namespace_of(n->inst)}) {
            for (const auto decl : ns->decls) {
                names.emplace_back(n->owner->uir->string(n->owner->uir->decl_at(decl).name));
            }
        }
    }
    for (const auto impl : applied) {
        if (impl->interface) { continue; }
        const auto& u{*impl->entry->owner};
        const auto& m{*u.uir};
        const auto  ns{m.namespace_of(impl->entry->inst)};
        if (!ns) { continue; }
        for (const auto decl : ns->decls) {
            const auto name{m.string(m.decl_at(decl).name)};
            if (std::ranges::contains(names, name)) {
                DISCARD(report(u,
                               source{*m.decl_at(decl).node},
                               gs::error::DUPLICATE_MEMBER,
                               fmt::format("`{}` is already a member of `{}`",
                                           name,
                                           ctx.type_display_name(target))));
                continue;
            }
            names.emplace_back(name);
        }
    }

    for (const auto impl : applied) { check_conformance(*impl); }
}

auto compilation::check_interface(nominal& iface) -> void {
    const auto& m{*iface.owner->uir};
    for (const auto& method : interface_methods(iface)) {
        // A method reading associated items is typed only for an impl that binds them
        if (method.closure) { continue; }
        analyzer sig{*this, *iface.owner};
        sig.enter(iface);
        record_reader rec{m, m.data_of(method.func).pl.extra};
        const auto    params{params_of(m, rec.body_field())};
        const auto    ret{rec.body_field()};
        // `@This()` is the implementing type, which the interface's own scope can't spell
        const auto names_this{[&](gsl::span<const inst_id> body) {
            return std::ranges::any_of(body, [&](inst_id i) {
                if (m.tag_of(i) != tag::builtin) { return false; }
                record_reader call{m, m.data_of(i).pl.extra};
                return static_cast<syntax::builtin_t>(call.u32_field()) == syntax::builtin_t::THIS;
            });
        }};
        const auto check{[&](gsl::span<const inst_id> body, const source& at) {
            if (body.empty() || names_this(body)) { return; }
            const auto type{sig.eval_type(body, at)};
            if (type) { DISCARD(sig.check_storable(**type, at)); }
        }};
        for (const auto param : params) {
            record_reader p{m, m.data_of(param).pl.extra};
            p.skip_str();
            check(p.body_field(), m.src_of(param));
            if (stopped_at_) { return; }
        }
        check(ret, m.src_of(method.func));
        if (stopped_at_) { return; }
    }
}

auto compilation::check_conformance(impl_instance& applied) -> void {
    if (!applied.interface) { return; }
    const auto iface{nominal_of(*applied.interface)};
    if (!iface) { return; }

    const auto& u{*applied.entry->owner};
    const auto& m{*u.uir};
    const auto& im{*iface->owner->uir};
    const auto& site{m.src_of(applied.entry->inst)};
    const auto  iface_name{ctx_.type_display_name(*applied.interface)};
    const auto  target_name{ctx_.type_display_name(*applied.target)};
    const auto  ns{m.namespace_of(applied.entry->inst)};
    const auto  written{[&](std::string_view name) -> stdx::option<decl_id> {
        if (!ns) { return stdx::none; }
        for (const auto decl : ns->decls) {
            if (m.string(m.decl_at(decl).name) == name) { return decl; }
        }
        return stdx::none;
    }};

    const auto methods{interface_methods(*iface)};
    for (const auto& method : methods) {
        const auto supplied{written(method.name)};
        if (!supplied) {
            if (is_bodyless(im, method.func)) {
                DISCARD(report(u,
                               site,
                               gs::error::MISSING_IMPL_METHOD,
                               fmt::format("`{}` does not implement `{}`: missing method `{}`",
                                           target_name,
                                           iface_name,
                                           method.name)));
            }
            continue;
        }

        const auto& d{m.decl_at(*supplied)};
        const auto  func{declared_func(m, d)};
        if (!func) { continue; }
        const source method_src{*d.node};
        const auto   want{shape_of_signature(im, method.func)};
        const auto   got{shape_of_signature(m, *func)};
        if (want.has_self != got.has_self || want.count != got.count) {
            DISCARD(report(u,
                           method_src,
                           gs::error::IMPL_SIGNATURE_MISMATCH,
                           fmt::format("method `{}` does not match the signature required by `{}` "
                                       "(parameter count or `self` differs)",
                                       method.name,
                                       iface_name)));
            continue;
        }

        if (want.has_self && !self_compatible(want.how, got.how)) {
            DISCARD(report(
                u,
                method_src,
                gs::error::IMPL_SELF_MISMATCH,
                fmt::format("method `{}`: `self` binding is not compatible with the requirement in "
                            "`{}`",
                            method.name,
                            iface_name)));
            continue;
        }

        // The types are compared where both spell them plainly, the method's own not generic
        if (method.closure) { continue; }
        const auto member{impl_member(applied, method.name, method_src)};
        const auto fn{member && *member ? (*member)->as_opt<function_ref>() : stdx::none};
        if (!fn) { continue; }
        const auto got_sig{fn->fn->type->get_data().as_opt<gs::types::function>()};
        if (!got_sig) { continue; }

        record_reader rec{im, im.data_of(method.func).pl.extra};
        const auto    params{params_of(im, rec.body_field())};
        const auto    ret{rec.body_field()};
        analyzer      sig{*this, *iface->owner};
        const auto    before{speculate()};
        const auto    eval{[&](gsl::span<const inst_id> body) -> stdx::option<gs::type&> {
            const auto type{sig.eval_type(body, im.src_of(method.func))};
            if (!type) { return stdx::none; }
            return **type;
        }};

        const usize first{want.has_self ? 1UZ : 0UZ};
        bool        mismatch{false};
        std::string what;
        for (usize p{first}; p < params.size() && p < got_sig->params.size() && !mismatch; ++p) {
            record_reader param{im, im.data_of(params[p]).pl.extra};
            param.skip_str();
            const auto wanted{eval(param.body_field())};
            if (!wanted) { break; }
            if (!gs::is_same_unqualified(*wanted, *got_sig->params[p])) {
                mismatch = true;
                what     = fmt::format("parameter {} type", p - first + 1);
            }
        }
        if (!mismatch) {
            if (const auto wanted{eval(ret)};
                wanted && !gs::is_same_unqualified(*wanted, got_sig->return_type)) {
                mismatch = true;
                what     = "return type";
            }
        }
        abandon(before);

        if (mismatch) {
            DISCARD(report(u,
                           method_src,
                           gs::error::IMPL_SIGNATURE_MISMATCH,
                           fmt::format("method `{}`: {} does not match the requirement in `{}`",
                                       method.name,
                                       what,
                                       iface_name)));
        }
    }

    // Every required associated type is bound by the impl or defaulted by the interface
    const auto types{associated_items(im, iface->inst, true)};
    const auto consts{associated_items(im, iface->inst, false)};
    for (const auto& item : types) {
        if (item.defaulted || written(item.name)) { continue; }
        DISCARD(report(u,
                       site,
                       gs::error::MISSING_IMPL_METHOD,
                       fmt::format("`{}` does not implement `{}`: associated type `{}` is not bound",
                                   target_name,
                                   iface_name,
                                   item.name)));
    }

    // A static `let mut` may hold the impl's state, but nothing else outside the interface
    if (!ns) { return; }
    for (const auto decl : ns->decls) {
        const auto& d{m.decl_at(decl)};
        const auto  name{m.string(d.name)};
        const bool  item{std::ranges::contains(methods, name, &interface_method::name) ||
                        std::ranges::contains(types, name, &associated_item::name) ||
                        std::ranges::contains(consts, name, &associated_item::name) ||
                        d.has(decl_flags::MUT)};
        if (item) { continue; }
        DISCARD(report(u,
                       site,
                       gs::error::UNKNOWN_IMPL_MEMBER,
                       fmt::format("`{}` is not a member of interface `{}`; put unrelated items in "
                                   "an inherent `impl {}` block",
                                   name,
                                   iface_name,
                                   target_name)));
    }
}

} // namespace ghoti::uir::sema
