#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/expression.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/ip/pool.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"

namespace ghoti::uir::sema {

namespace {

// Where a `dyn` value keeps its object's and its vtable's address
constexpr u64 DYN_DATA_FIELD{0};
constexpr u64 DYN_VTABLE_FIELD{1};

// The `dyn I` a pointer or reference points at, if it points at one
[[nodiscard]] auto dyn_behind(const gs::type& t) -> stdx::option<gs::type&> {
    const auto pointee{gs::pointee_of(t)};
    if (!pointee || pointee->get_kind() != gs::type_kind::DYN) { return stdx::none; }
    return *pointee;
}

} // namespace

auto compilation::method_has_defaults(const nominal& iface, std::string_view name) -> bool {
    const auto& m{*iface.owner->uir};
    for (const auto& method : interface_methods(iface)) {
        if (method.name != name || m.tag_of(method.func) != tag::func) { continue; }
        record_reader fn{m, m.data_of(method.func).pl.extra};
        for (const auto param : params_of(m, fn.body_field())) {
            record_reader p{m, m.data_of(param).pl.extra};
            p.skip_str().skip_body();
            if (!p.body_field().empty()) { return true; }
        }
    }

    return false;
}

auto compilation::defaulted_method(const nominal& iface, std::string_view name, usize written)
    -> std::string {
    if (!method_has_defaults(iface, name)) { return std::string{name}; }
    auto       wrapper{ast::default_wrapper_name(name, written)};
    const auto methods{interface_methods(iface)};
    if (std::ranges::contains(methods, wrapper, &interface_method::name)) { return wrapper; }
    return std::string{name};
}

auto compilation::interface_methods(const nominal& iface) -> std::vector<interface_method> {
    const auto&   m{*iface.owner->uir};
    record_reader rec{m, m.data_of(iface.inst).pl.extra};
    for (u32 i{0}, count{rec.count()}; i < count; ++i) { rec.skip_ref().skip_u32().skip_str(); }
    rec.skip_body();
    for (u32 list{0}; list < 2; ++list) {
        for (u32 i{0}, count{rec.count()}; i < count; ++i) {
            rec.skip_str().skip_body().skip_body();
        }
    }

    std::vector<interface_method> methods;
    for (u32 i{0}, count{rec.count()}; i < count; ++i) {
        const auto name{rec.str_field()};
        rec.skip_u32();
        const auto signature{rec.body_field()};
        const auto attrs{rec.body_field()};
        const auto yielded{signature.empty() || m.tag_of(signature.back()) != tag::yield
                               ? stdx::none
                               : as_inst(m.data_of(signature.back()).un.operand)};
        if (!yielded) { continue; }

        // A method reading an associated item is a closure over it
        if (m.tag_of(*yielded) == tag::closure) {
            record_reader closure{m, m.data_of(*yielded).pl.extra};
            const auto    func{as_inst(*closure.ref_field())};
            if (!func) { continue; }
            methods.emplace_back<interface_method>(
                {.name = name, .func = *func, .closure = *yielded, .attrs = attrs});
            continue;
        }

        methods.emplace_back<interface_method>({.name = name, .func = *yielded, .attrs = attrs});
    }

    return methods;
}

auto compilation::vtable_for(impl_instance& applied) -> result<std::string_view> {
    if (applied.vtable) { return std::string_view{*applied.vtable}; }
    const auto iface{nominal_of(*applied.interface)};

    // Every method goes in the vtable, so each one's body is analyzed now
    const auto& impl_unit{*applied.entry->owner}; // The coercion asking may be in another module
    const auto& at_impl{impl_unit.uir->src_of(applied.entry->inst)};

    // A parameterized impl's vtable is named for the instance it serves
    mod::dyn_vtable vtable{.symbol = applied.prefix.empty()
                                         ? fmt::format("__vtable.{}", vtables_++)
                                         : fmt::format("__vtable.{}", applied.prefix),
                           .slots  = {}};
    for (const auto& method : interface_methods(*iface)) {
        const auto member{TRY(impl_member(applied, method.name, at_impl))};
        const auto fn{member ? member->as_opt<function_ref>() : stdx::none};
        if (!fn) { return unsupported(impl_unit, at_impl, "a vtable slot that isn't a method"); }

        request_body(*fn->fn);
        vtable.slots.emplace_back(fn->fn->symbol);
    }

    applied.vtable.emplace(vtable.symbol);
    root_source().dyn_vtables.emplace_back(std::move(vtable));
    return std::string_view{*applied.vtable};
}

auto compilation::declares_associated_items(const nominal& iface) -> bool {
    const auto&   m{*iface.owner->uir};
    record_reader rec{m, m.data_of(iface.inst).pl.extra};
    for (u32 i{0}, count{rec.count()}; i < count; ++i) { rec.skip_ref().skip_u32().skip_str(); }
    rec.skip_body();

    // With no associated types to read past, the next list is the associated constants
    if (rec.count() != 0) { return true; }
    return rec.count() != 0;
}

auto compilation::defaults_every_associated_item(const nominal& iface) -> bool {
    const auto&   m{*iface.owner->uir};
    record_reader rec{m, m.data_of(iface.inst).pl.extra};
    for (u32 i{0}, count{rec.count()}; i < count; ++i) { rec.skip_ref().skip_u32().skip_str(); }
    rec.skip_body();
    for (u32 list{0}; list < 2; ++list) {
        for (u32 i{0}, count{rec.count()}; i < count; ++i) {
            rec.skip_str().skip_body();
            if (rec.body_field().empty()) { return false; }
        }
    }

    return true;
}

auto compilation::associated_value(const nominal&               iface,
                                   stdx::option<impl_instance&> applied,
                                   std::string_view             name,
                                   const source&                use) -> result<ip::index> {
    const auto& owner{*iface.owner};
    if (applied) {
        if (const auto member{TRY(impl_member(*applied, name, use))}) {
            if (const auto index{member->as_opt<ip::index>()}) { return *index; }
            return unsupported(owner, use, "an associated item that isn't known at compile time");
        }
    }

    // The interface's default, read in its own module
    const auto&   m{*owner.uir};
    record_reader rec{m, m.data_of(iface.inst).pl.extra};
    for (u32 i{0}, count{rec.count()}; i < count; ++i) { rec.skip_ref().skip_u32().skip_str(); }
    rec.skip_body();
    for (u32 list{0}; list < 2; ++list) {
        for (u32 i{0}, count{rec.count()}; i < count; ++i) {
            const auto item{rec.str_field()};
            rec.skip_body();
            const auto fallback{rec.body_field()};
            if (item != name || fallback.empty()) { continue; }
            analyzer    a{*this, owner};
            const auto& at{m.src_of(fallback.back())};
            if (list == 0) { return TRY(a.eval_type(fallback, at))->get_ip_index(); }
            const auto value{TRY(a.eval_comptime(fallback))};
            if (const auto index{value.as_opt<ip::index>()}) { return *index; }
            return unsupported(
                owner, at, "an associated constant that isn't known at compile time");
        }
    }

    // Reported where the interface is, since `use` may be in another module
    DISCARD(use);
    return unsupported(
        owner, m.src_of(iface.inst), fmt::format("an associated item `{}` with no value", name));
}

auto compilation::associated_captures(const nominal&               iface,
                                      inst_id                      closure,
                                      stdx::option<impl_instance&> applied,
                                      const source& use) -> result<std::vector<ip::index>> {
    const auto&   m{*iface.owner->uir};
    record_reader rec{m, m.data_of(closure).pl.extra};
    rec.skip_ref();
    std::vector<ip::index> values;
    for (u32 i{0}, count{rec.count()}; i < count; ++i) {
        rec.skip_ref().skip_u32();
        values.emplace_back(TRY(associated_value(iface, applied, rec.str_field(), use)));
    }

    return values;
}

auto compilation::root_source() -> mod::module_t& {
    for (const auto& u : units_) {
        if (u.source.get() == prog_.root.get()) { return *u.source; }
    }

    return *units_.front().source;
}

auto analyzer::analyze_dyn_type(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    auto&       iface{*TRY(resolve_type(*rec.ref_field(), src))};

    // `dyn I(Assoc = T)` binds associated types by name, a later binding of one replacing it
    std::vector<std::pair<std::string_view, gsl::not_null<gs::type*>>> bound;
    const auto                                                         count{rec.count()};
    for (u32 i{0}; i < count; ++i) {
        const auto name{rec.str_field()};
        const auto type_ref{*rec.ref_field()};
        const auto at{
            as_inst(type_ref).transform([&](inst_id t) { return src_of(t); }).value_or(src)};
        bound.emplace_back(name, TRY(resolve_type(type_ref, at)));
    }

    if (iface.get_kind() != gs::type_kind::INTERFACE) {
        return comp_.unsupported(u_, src, "a `dyn` of something other than an interface");
    }

    const auto n{comp_.nominal_of(iface)};
    if (!n) { return comp_.unsupported(u_, src, "a `dyn` of an interface UIR didn't declare"); }
    auto&       ctx{comp_.ctx()};
    const auto& names{iface.get_data().as<gs::types::interface_t>().assoc_type_names};
    for (const auto& [name, type] : bound) {
        if (!std::ranges::contains(names, name)) {
            return comp_.unsupported(u_, src, "a `dyn` binding an associated type it doesn't have");
        }
    }

    // Each associated type is bound as written, else to its default
    const auto written{[&](std::string_view name) -> stdx::option<gs::type&> {
        for (const auto& [bound_name, type] : std::views::reverse(bound)) {
            if (bound_name == name) { return *type; }
        }

        return stdx::none;
    }};
    const bool every_bound{
        std::ranges::all_of(names, [&](auto name) { return written(name).has_value(); })};
    if (!every_bound && comp_.declares_associated_items(*n) &&
        !comp_.defaults_every_associated_item(*n)) {
        return comp_.unsupported(u_, src, "a `dyn` of an associated item with no default");
    }

    auto bindings{ctx.pool.get_many_unsafe(names.size())};
    for (usize i{0}; i < names.size(); ++i) {
        if (const auto type{written(names[i])}) {
            bindings[i] = &*type;
            continue;
        }

        const auto value{TRY(comp_.associated_value(*n, stdx::none, names[i], src))};
        bindings[i] = &comp_.constants().type_at(value);
    }

    return interned_type(ctx.get_dyn(iface, bindings));
}

auto analyzer::coerce_to_dyn(const operand& op, gs::type& target, const source& src)
    -> result<stdx::option<operand>> {
    const auto dyn{dyn_behind(target)};
    if (!dyn) { return stdx::option<operand>{}; }
    auto&      from{type_of(op)};
    const auto concrete{gs::pointee_of(from)};
    if (!concrete || !comp_.nominal_of(*concrete)) { return stdx::option<operand>{}; }

    auto&      iface{dyn->get_data().as<gs::types::dyn_t>().interface};
    const auto impls{TRY(comp_.impls_of(*concrete))};
    const auto applied{
        std::ranges::find_if(impls, [&](const auto impl) { return impl->interface == iface; })};
    if (applied == impls.end()) { return stdx::option<operand>{}; }
    // A global's address and its vtable are both symbols, so the pair is a constant
    if (is_comptime()) {
        const auto known{comptime_value(op)};
        const auto address{known
                               ? comp_.constants().value_at(*known).data.as_opt<ip::address_value>()
                               : stdx::none};
        if (!address || !address->pointee.empty()) {
            return comp_.unsupported(u_, src, "a `dyn` value at compile time");
        }

        std::string data_symbol{address->symbol};
        const auto  vtable{TRY(comp_.vtable_for(**applied))};
        return stdx::option<operand>{
            comp_.constants().intern(ip::dyn_value{.data_symbol   = std::move(data_symbol),
                                                   .vtable_symbol = std::string{vtable}},
                                     target)};
    }

    const auto vtable{TRY(comp_.vtable_for(**applied))};
    TRY(require_runtime(src));

    // `{ data, vtable }` is built in a slot typed by the bare `dyn`, then read out
    auto&      ctx{comp_.ctx()};
    auto&      opaque_ptr{ctx.get_pointer(gs::types::mut::CONSTANT,
                                          ctx.get_builtin_resolved_type(gs::type_kind::OPAQUE))};
    const auto data{TRY(as_runtime(op, src))};
    const auto table{builder_.emit_global_addr(std::string{vtable}, opaque_ptr, true)};
    const auto slot{builder_.emit_alloca(*dyn)};
    const auto data_at{builder_.emit_get_element_ptr(
        gir::value{slot, *dyn}, {usize_constant(DYN_DATA_FIELD)}, opaque_ptr)};
    builder_.emit_store(gir::value{data_at, opaque_ptr}, gir::value{data.data, opaque_ptr})
        .is_initializer = true;
    const auto table_at{builder_.emit_get_element_ptr(
        gir::value{slot, *dyn}, {usize_constant(DYN_VTABLE_FIELD)}, opaque_ptr)};
    builder_.emit_store(gir::value{table_at, opaque_ptr}, gir::value{table, opaque_ptr})
        .is_initializer = true;
    return stdx::option<operand>{
        operand{gir::value{builder_.emit_load(gir::value{slot, *dyn}, target), target}}};
}

auto analyzer::call_dyn(inst_id              call,
                        const operand&       object,
                        std::string_view     name,
                        gsl::span<const ref> args,
                        const source&        src) -> result<stdx::option<operand>> {
    auto&      fat_type{type_of(object)};
    const auto dyn{dyn_behind(fat_type)};
    if (!dyn) { return stdx::option<operand>{}; }

    auto&      iface_type{dyn->get_data().as<gs::types::dyn_t>().interface};
    const auto iface{comp_.nominal_of(iface_type)};
    if (!iface) { return comp_.unsupported(u_, src, "a `dyn` of an interface UIR didn't declare"); }

    // A call leaving defaulted arguments off goes through the wrapper's slot
    const auto methods{comp_.interface_methods(*iface)};
    const auto chosen{comp_.defaulted_method(*iface, name, args.size())};
    const auto slot{std::ranges::find(methods, chosen, &interface_method::name)};
    if (slot == methods.end()) {
        return comp_.report(u_,
                            src,
                            gs::error::UNDECLARED_IDENTIFIER,
                            fmt::format("Type '{}' has no field named '{}'",
                                        comp_.ctx().type_display_name(fat_type),
                                        name));
    }

    // The method's parameter and return types come from the interface's own signature
    const auto&   owner{*iface->owner};
    const auto&   m{*owner.uir};
    record_reader rec{m, m.data_of(slot->func).pl.extra};
    const auto    params{params_of(m, rec.body_field())};
    const auto    ret{rec.body_field()};
    analyzer      sig{comp_, owner};
    // An associated type the `dyn` binds is that binding, any other its default
    if (slot != methods.end() && slot->closure) {
        const auto&   bound{dyn->get_data().as<gs::types::dyn_t>().assoc_bindings};
        const auto&   names{iface_type.get_data().as<gs::types::interface_t>().assoc_type_names};
        record_reader closure_rec{m, m.data_of(*slot->closure).pl.extra};
        std::vector<ip::index> captures;
        closure_rec.skip_ref();
        for (u32 i{0}, count{closure_rec.count()}; i < count; ++i) {
            closure_rec.skip_ref().skip_u32();
            const auto item{closure_rec.str_field()};
            const auto at{std::ranges::find(names, item)};
            if (at != names.end() && static_cast<usize>(at - names.begin()) < bound.size()) {
                captures.emplace_back(
                    bound[static_cast<usize>(at - names.begin())]->get_ip_index());
                continue;
            }

            captures.emplace_back(TRY(comp_.associated_value(*iface, stdx::none, item, src)));
        }

        sig.enter(member_context{.captures = std::move(captures)});
    }

    if (params.size() != args.size() + 1) {
        return comp_.report(
            u_,
            src,
            gs::error::ARITY_MISMATCH,
            fmt::format("Expected {} arguments, found {}", params.size() - 1, args.size()));
    }

    TRY(require_runtime(src));
    const auto index{static_cast<u64>(slot - methods.begin())};
    const auto loaded{[&] -> result<dyn_entry> {
        if (const auto found{dyn_entries_.find(call)}; found != dyn_entries_.end()) {
            return found->second;
        }

        return load_dyn_entry(object, *dyn, index, src);
    }()};
    if (!loaded) { return stdx::err{loaded.error()}; }
    std::vector<gir::value> values;
    std::vector<gs::type*>  param_types{&loaded->data.type.value()};
    values.emplace_back(loaded->data);
    for (usize i{0}; i < args.size(); ++i) {
        record_reader p{m, m.data_of(params[i + 1]).pl.extra};
        p.skip_str();
        auto& param_type{*TRY(sig.eval_type(p.body_field(), m.src_of(params[i + 1])))};
        param_types.emplace_back(&param_type);
        const auto arg{TRY(resolve_value(args[i], src))};
        const auto coerced{TRY(coerce_to(
            arg,
            param_type,
            src,
            {.what = use_site::kind::ARGUMENT, .arg_index = i + 1, .callee = slot->name}))};
        values.emplace_back(TRY(as_runtime(coerced, src)));
    }

    auto& ret_type{*TRY(sig.eval_type(ret, m.src_of(slot->func)))};

    // The entry is called as the method's own signature, taking the data pointer as `self`
    auto& ctx{comp_.ctx()};
    auto  span{ctx.pool.get_many_unsafe(param_types.size())};
    std::ranges::copy(param_types, span.begin());
    auto&      fn_type{ctx.get_function(span, ret_type, false, ast::calling_convention::C, false)};
    const auto result{builder_.emit_indirect_call(
        gir::value{loaded->entry.data, fn_type}, std::move(values), ret_type)};
    if (!result) { return stdx::option<operand>{void_value()}; }
    return stdx::option<operand>{operand{gir::value{*result, ret_type}}};
}

auto analyzer::load_dyn_entry(const operand& object, gs::type& dyn, u64 index, const source& src)
    -> result<dyn_entry> {
    auto& ctx{comp_.ctx()};
    auto& opaque_ptr{ctx.get_pointer(gs::types::mut::CONSTANT,
                                     ctx.get_builtin_resolved_type(gs::type_kind::OPAQUE))};
    // A field holding the `dyn` is read from where it lives
    const auto fat{TRY([&] -> result<gir::value> {
        if (const auto p{object.as_opt<place>()}) { return read(*p, src); }
        return as_runtime(object, src);
    }())};
    const auto spilled{builder_.emit_alloca(dyn)};
    builder_.emit_store(gir::value{spilled, dyn}, fat);
    const auto data_at{builder_.emit_get_element_ptr(
        gir::value{spilled, dyn}, {usize_constant(DYN_DATA_FIELD)}, opaque_ptr)};
    const auto table_at{builder_.emit_get_element_ptr(
        gir::value{spilled, dyn}, {usize_constant(DYN_VTABLE_FIELD)}, opaque_ptr)};
    const auto data{builder_.emit_load(gir::value{data_at, opaque_ptr}, opaque_ptr)};
    const auto table{builder_.emit_load(gir::value{table_at, opaque_ptr}, opaque_ptr)};
    // The table is an array of code pointers
    auto&      table_ptr{ctx.get_pointer(gs::types::mut::CONSTANT, opaque_ptr)};
    const auto entry_at{builder_.emit_get_element_ptr(
        gir::value{table, table_ptr}, {usize_constant(index)}, opaque_ptr)};
    const auto entry{builder_.emit_load(gir::value{entry_at, opaque_ptr}, opaque_ptr)};
    return dyn_entry{.data = gir::value{data, opaque_ptr}, .entry = gir::value{entry, opaque_ptr}};
}

void analyzer::prepare_dyn_call(inst_id call) {
    auto       rec{record(call)};
    const auto object_ref{*rec.ref_field()};
    const auto object_inst{as_inst(object_ref)};
    if (object_inst && !values_.contains(*object_inst)) { return; }
    const auto object{resolve(object_ref, src_of(call))};
    if (!object) { return; }
    const auto dyn{dyn_behind(type_of(*object))};
    if (!dyn || is_comptime()) { return; }
    const auto name{rec.str_field()};
    rec.skip_u32();
    if (rec.ref_field()) { return; }
    const auto written{rec.refs_field().size()};

    // Only a call the vtable can make is prepared, so a bad one still reports at the call
    const auto iface{comp_.nominal_of(dyn->get_data().as<gs::types::dyn_t>().interface)};
    if (!iface) { return; }
    const auto methods{comp_.interface_methods(*iface)};
    const auto chosen{comp_.defaulted_method(*iface, name, written)};
    const auto slot{std::ranges::find(methods, chosen, &interface_method::name)};
    if (slot == methods.end()) { return; }
    const auto index{static_cast<u64>(slot - methods.begin())};
    if (const auto loaded{load_dyn_entry(*object, *dyn, index, src_of(call))}) {
        dyn_entries_.insert_or_assign(call, *loaded);
    }
}

} // namespace ghoti::uir::sema
