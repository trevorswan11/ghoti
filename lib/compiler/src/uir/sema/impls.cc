#include <algorithm>
#include <array>
#include <bit>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ip/index.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"

namespace ghoti::uir::sema {

namespace {

// An impl's parameter list, target body and interface body
struct impl_shape {
    gsl::span<const inst_id> params;
    gsl::span<const inst_id> interface;
    gsl::span<const inst_id> target;
};

[[nodiscard]] auto shape_of(const module_t& m, inst_id impl) -> impl_shape {
    record_reader rec{m, m.data_of(impl).pl.extra};
    const auto    params{rec.body_field()};
    const auto    interface{rec.body_field()};
    const auto    target{rec.body_field()};
    return impl_shape{.params = params, .interface = interface, .target = target};
}

// The instruction a body yields, if it ends in a `yield` of one
[[nodiscard]] auto yielded_inst(const module_t& m, gsl::span<const inst_id> body)
    -> stdx::option<inst_id> {
    if (body.empty() || m.tag_of(body.back()) != tag::yield) { return stdx::none; }
    return as_inst(m.data_of(body.back()).un.operand);
}

} // namespace

auto impl_key_hash::operator()(const impl_key& key) const noexcept -> u64 {
    stdx::hasher h{static_cast<u64>(std::bit_cast<uptr>(key.entry.get()))};
    h.combine(static_cast<u64>(std::bit_cast<uptr>(key.target.get())));
    return h.finalize();
}

auto compilation::register_impl(const unit& u, decl_id decl) -> void {
    const auto& d{u.uir->decl_at(decl)};
    const auto  impl{yielded_inst(*u.uir, u.uir->body(d.value_body))};
    if (!impl || u.uir->tag_of(*impl) != tag::impl_decl) { return; }
    impls_.emplace_back(arena_.make<impl_entry>(impl_entry{
        .owner = &units_[u.index],
        .decl  = decl,
        .inst  = *impl,
    }));
}

auto compilation::analyze_impl_roots(const unit& u, decl_id decl) -> void {
    const auto entry{std::ranges::find_if(impls_, [&](const auto& e) {
        return e->owner.get() == &units_[u.index] && e->decl == decl;
    })};
    if (entry == impls_.end()) { return; }

    // A parameterized impl's members are analyzed only for the targets that use them
    const auto& m{*u.uir};
    const auto  shape{shape_of(m, (*entry)->inst)};
    if (!shape.params.empty()) { return; }

    analyzer   a{*this, u};
    const auto target{a.eval_type(shape.target, m.src_of((*entry)->inst))};
    if (!target) { return; }
    const auto applied{impls_of(**target)};
    if (!applied) { return; }
    for (const auto impl : *applied) {
        if (impl->entry.get() != entry->get()) { continue; }
        const auto ns{m.namespace_of(impl->entry->inst)};
        if (!ns) { return; }
        for (const auto member : ns->decls) {
            const auto& d{m.decl_at(member)};
            if (d.kind != decl_kind::VALUE || is_deferred_error(u, d)) { continue; }
            DISCARD(analyze_impl_decl(*impl, member, *d.node));
            if (stopped_at_) { return; }
        }

        // The interface's defaults the impl doesn't write are its members too
        const auto iface{impl->interface ? nominal_of(*impl->interface) : stdx::none};
        if (!iface) { continue; }
        for (const auto& method : interface_methods(*iface)) {
            const bool written{std::ranges::any_of(ns->decls, [&](decl_id member) {
                return m.string(m.decl_at(member).name) == method.name;
            })};
            if (written) { continue; }
            DISCARD(default_method(*impl, method.name));
            if (stopped_at_) { return; }
        }
    }
}

auto compilation::impl_params_for(const impl_entry& entry, gs::type& target)
    -> result<stdx::option<std::vector<ip::index>>> {
    const auto& m{*entry.owner->uir};
    const auto  shape{shape_of(m, entry.inst)};
    const auto& src{m.src_of(entry.inst)};

    // A plain impl names its target outright
    if (shape.params.empty()) {
        analyzer   a{*this, *entry.owner};
        const auto named{TRY(a.eval_type(shape.target, src))};
        if (named.get() != &target) { return stdx::option<std::vector<ip::index>>{}; }
        return stdx::option<std::vector<ip::index>>{std::vector<ip::index>{}};
    }

    // A parameterized impl matches the instances of the ctor it calls with its parameters
    const auto n{nominal_of(target)};
    if (!n || !n->creator) { return stdx::option<std::vector<ip::index>>{}; }
    const auto call{yielded_inst(m, shape.target)};
    if (!call || m.tag_of(*call) != tag::call) {
        return unsupported(*entry.owner, src, "an impl target that isn't a ctor call");
    }

    record_reader rec{m, m.data_of(*call).pl.extra};
    const auto    callee{as_inst(*rec.ref_field())};
    rec.skip_ref();
    const auto args{rec.refs_field()};
    if (!callee || m.tag_of(*callee) != tag::decl_val) {
        return unsupported(*entry.owner, src, "an impl target whose ctor isn't a declaration");
    }

    const auto&   creator{*n->creator};
    const decl_id ctor_decl{m.data_of(*callee).un.aux};
    if (creator.owner.get() != entry.owner.get() || creator.decl != ctor_decl) {
        return stdx::option<std::vector<ip::index>>{};
    }

    std::vector<ip::index> values(shape.params.size(), ip::index{});
    for (usize j{0}; j < args.size() && j < creator.bindings.size(); ++j) {
        const auto arg{as_inst(args[j])};
        const auto param{arg ? std::ranges::find(shape.params, *arg) : shape.params.end()};
        if (param == shape.params.end() || !creator.bindings[j].value) {
            return unsupported(*entry.owner, src, "an impl target with fixed ctor arguments");
        }

        values[static_cast<usize>(param - shape.params.begin())] = *creator.bindings[j].value;
    }

    return stdx::option<std::vector<ip::index>>{std::move(values)};
}

auto compilation::impls_of(gs::type& target) -> result<std::vector<gsl::not_null<impl_instance*>>> {
    std::vector<gsl::not_null<impl_instance*>> found;
    for (const auto entry : impls_) {
        const impl_key key{.entry = entry, .target = &target};
        if (const auto known{impl_instances_.find(key)}; known != impl_instances_.end()) {
            if (known->second) { found.emplace_back(known->second.get()); }
            continue;
        }

        const auto params{TRY(impl_params_for(*entry, target))};
        if (!params) {
            impl_instances_.emplace(key, stdx::none);
            continue;
        }

        auto& applied{*arena_.make<impl_instance>(impl_instance{
            .entry     = entry,
            .target    = &target,
            .params    = std::move(*params),
            .interface = TRY(impl_interface(*entry)),
            .prefix    = impl_symbol_prefix(*entry, target),
        })};
        impl_instances_.emplace(key, applied);
        found.emplace_back(&applied);
    }

    return found;
}

auto compilation::impl_interface(const impl_entry& entry) -> result<stdx::option<gs::type&>> {
    const auto& m{*entry.owner->uir};
    const auto  shape{shape_of(m, entry.inst)};
    if (shape.interface.empty()) { return stdx::option<gs::type&>{}; }
    analyzer a{*this, *entry.owner};
    auto&    interface{*TRY(a.eval_type(shape.interface, m.src_of(entry.inst)))};
    return stdx::option<gs::type&>{interface};
}

auto compilation::impl_symbol_prefix(const impl_entry& entry, gs::type& target) -> std::string {
    // A parameterized impl's methods are named for the instance they serve
    const auto n{nominal_of(target)};
    const auto shape{shape_of(*entry.owner->uir, entry.inst)};
    if (shape.params.empty() || !n) { return {}; }
    return fmt::format("pimpl{}#{}", std::to_underlying(entry.inst), n->symbol_prefix);
}

auto compilation::enter_impl(impl_instance& applied, analyzer& a) -> void {
    const auto shape{shape_of(*applied.entry->owner->uir, applied.entry->inst)};
    a.enter(impl_context(applied));
    for (usize i{0}; i < shape.params.size(); ++i) {
        a.bind(shape.params[i], operand{applied.params[i]});
    }
}

auto compilation::impl_context(impl_instance& applied) -> member_context {
    return member_context{
        .captures      = {},
        .self_type     = *applied.target,
        .symbol_prefix = applied.prefix,
        .scope         = nominal_of(*applied.target),
    };
}

auto compilation::impl_member(impl_instance& applied, std::string_view name, const source& use)
    -> result<stdx::option<operand>> {
    const auto& u{*applied.entry->owner};
    const auto  iface{applied.interface ? nominal_of(*applied.interface) : stdx::none};
    if (iface && method_has_defaults(*iface, name)) {
        return unsupported(
            u, u.uir->src_of(applied.entry->inst), "an interface method with default arguments");
    }

    const auto& m{*u.uir};
    const auto  ns{m.namespace_of(applied.entry->inst)};
    if (ns) {
        for (const auto decl : ns->decls) {
            if (m.string(m.decl_at(decl).name) != name) { continue; }
            return stdx::option<operand>{TRY(analyze_impl_decl(applied, decl, use))};
        }
    }

    return default_method(applied, name);
}

auto compilation::default_method(impl_instance& applied, std::string_view name)
    -> result<stdx::option<operand>> {
    if (!applied.interface) { return stdx::option<operand>{}; }
    const auto iface{nominal_of(*applied.interface)};
    if (!iface) { return stdx::option<operand>{}; }

    const auto&   u{*iface->owner};
    const auto&   m{*u.uir};
    record_reader rec{m, m.data_of(iface->inst).pl.extra};
    for (u32 i{0}, count{rec.count()}; i < count; ++i) { rec.skip_ref().skip_u32().skip_str(); }
    rec.skip_body();
    for (u32 list{0}; list < 2; ++list) {
        for (u32 i{0}, count{rec.count()}; i < count; ++i) {
            rec.skip_str().skip_body().skip_body();
        }
    }

    for (u32 i{0}, count{rec.count()}; i < count; ++i) {
        const auto method{rec.str_field()};
        rec.skip_u32();
        const auto signature{rec.body_field()};
        rec.skip_body();
        if (method != name) { continue; }
        if (const auto known{applied.defaults.find(i)}; known != applied.defaults.end()) {
            return stdx::option<operand>{known->second};
        }

        // A method reading the interface's associated items closes over them
        auto func{yielded_inst(m, signature)};
        if (!func) { return stdx::option<operand>{}; }
        std::vector<ip::index> captures;
        if (m.tag_of(*func) == tag::closure) {
            captures = TRY(associated_captures(*iface, *func, applied, m.src_of(*func)));
            record_reader closure{m, m.data_of(*func).pl.extra};
            func = as_inst(*closure.ref_field());
        }

        if (!func || m.tag_of(*func) != tag::func) { return stdx::option<operand>{}; }

        record_reader fn{m, m.data_of(*func).pl.extra};
        fn.skip_body().skip_body().skip_body().skip_u32();
        if ((fn.u32_field() & flags::BODYLESS) != 0) { return stdx::option<operand>{}; }

        // A plain impl's default is named in the impl's scope, like the methods it writes out
        auto member{impl_context(applied)};
        member.captures = std::move(captures);
        const auto&       impl_unit{*applied.entry->owner};
        const std::string symbol{
            applied.prefix.empty()
                ? TRY(scoped_symbol(impl_unit, applied.entry->inst, method, u, m.src_of(*func)))
                : fmt::format("{}.{}", applied.prefix, method)};
        member.symbol = symbol;
        const auto owner_decl{*iface->declaring_decl};
        const auto value{TRY(declare_function(u, owner_decl, *func, member))};
        applied.defaults.emplace(i, value);
        return stdx::option<operand>{value};
    }

    return stdx::option<operand>{};
}

auto analyzer::analyze_interface_decl(inst_id inst) -> result<operand> {
    if (!is_comptime()) {
        return comp_.unsupported(u_, src_of(inst), "an interface declared in a runtime body");
    }

    auto& n{comp_.declare_nominal(u_, inst, gs::type_kind::INTERFACE)};

    // Only a `dyn`'s name reads the payload, so it lists only the associated items' names
    if (n.state == nominal_state::DECLARED) {
        auto&         ctx{comp_.ctx()};
        record_reader rec{record(inst)};
        for (u32 i{0}, count{rec.count()}; i < count; ++i) { rec.skip_ref().skip_u32().skip_str(); }
        rec.skip_body();
        std::array<gsl::span<std::string_view>, 2> names;
        for (auto& list : names) {
            const auto count{rec.count()};
            list = make_uninit_span<std::string_view>(ctx.arena, count);
            for (u32 i{0}; i < count; ++i) {
                list[i] = comp_.lasting_name(rec.str_field());
                rec.skip_body().skip_body();
            }
        }

        n.type->resolve<gs::type::data_t>(gs::types::interface_t{
            .method_sigs        = {},
            .method_src_indices = {},
            .method_names       = {},
            .method_is_pub      = {},
            .requirement_count  = 0,
            .assoc_type_names   = names[0],
            .assoc_const_names  = names[1],
            .ast_methods        = {},
            .ast_assoc_types    = {},
            .ast_assoc_consts   = {},
            .enclosing          = *u_.source,
        });
    }

    n.state = nominal_state::RESOLVED;
    if (!n.declaring_decl) { n.declaring_decl = declaring_; }
    return interned_type(*n.type);
}

} // namespace ghoti::uir::sema
