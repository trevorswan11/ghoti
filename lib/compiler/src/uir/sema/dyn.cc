#include <algorithm>
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

#include "compiler/gir/instruction.hh"
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
        rec.skip_body();
        const auto func{signature.empty() || m.tag_of(signature.back()) != tag::yield
                            ? stdx::none
                            : as_inst(m.data_of(signature.back()).un.operand)};
        if (!func) { continue; }
        methods.emplace_back<interface_method>({.name = name, .func = *func});
    }

    return methods;
}

auto compilation::vtable_for(impl_instance& applied) -> result<std::string_view> {
    if (applied.vtable) { return std::string_view{*applied.vtable}; }
    const auto iface{nominal_of(*applied.interface)};

    // Every method goes in the vtable, so each one's body is analyzed now, located at the impl
    // since the coercion asking may be in another module
    const auto&     impl_unit{*applied.entry->owner};
    const auto&     at_impl{impl_unit.uir->src_of(applied.entry->inst)};
    mod::dyn_vtable vtable{.symbol = fmt::format("__vtable.{}", vtables_++), .slots = {}};
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
    if (rec.count() != 0) { return comp_.unsupported(u_, src, "a `dyn` with associated types"); }
    if (iface.get_kind() != gs::type_kind::INTERFACE) {
        return comp_.unsupported(u_, src, "a `dyn` of something other than an interface");
    }

    const auto n{comp_.nominal_of(iface)};
    if (!n || comp_.declares_associated_items(*n)) {
        return comp_.unsupported(u_, src, "a `dyn` of an interface with associated items");
    }

    return interned_type(comp_.ctx().get_dyn(iface, {}));
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
    const auto applied{std::ranges::find_if(
        impls, [&](const auto impl) { return impl->interface && &*impl->interface == &iface; })};
    if (applied == impls.end()) { return stdx::option<operand>{}; }
    if (is_comptime()) { return comp_.unsupported(u_, src, "a `dyn` value at compile time"); }

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

auto analyzer::call_dyn(const operand&       object,
                        std::string_view     name,
                        gsl::span<const ref> args,
                        const source&        src) -> result<stdx::option<operand>> {
    auto&      fat_type{type_of(object)};
    const auto dyn{dyn_behind(fat_type)};
    if (!dyn) { return stdx::option<operand>{}; }

    auto&      iface_type{dyn->get_data().as<gs::types::dyn_t>().interface};
    const auto iface{comp_.nominal_of(iface_type)};
    if (!iface) { return comp_.unsupported(u_, src, "a `dyn` of an interface UIR didn't declare"); }

    const auto methods{comp_.interface_methods(*iface)};
    const auto slot{std::ranges::find(methods, name, &interface_method::name)};
    if (comp_.method_has_defaults(*iface, name)) {
        return comp_.unsupported(u_, src, "an interface method with default arguments");
    }

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
    if (params.size() != args.size() + 1) {
        return comp_.report(
            u_,
            src,
            gs::error::ARITY_MISMATCH,
            fmt::format("Expected {} arguments, found {}", params.size() - 1, args.size()));
    }

    std::vector<gir::value> values;
    TRY(require_runtime(src));
    auto&      ctx{comp_.ctx()};
    auto&      opaque_ptr{ctx.get_pointer(gs::types::mut::CONSTANT,
                                          ctx.get_builtin_resolved_type(gs::type_kind::OPAQUE))};
    const auto fat{TRY(as_runtime(object, src))};
    const auto spilled{builder_.emit_alloca(*dyn)};
    builder_.emit_store(gir::value{spilled, *dyn}, fat);
    const auto data_at{builder_.emit_get_element_ptr(
        gir::value{spilled, *dyn}, {usize_constant(DYN_DATA_FIELD)}, opaque_ptr)};
    const auto table_at{builder_.emit_get_element_ptr(
        gir::value{spilled, *dyn}, {usize_constant(DYN_VTABLE_FIELD)}, opaque_ptr)};
    const auto data{builder_.emit_load(gir::value{data_at, opaque_ptr}, opaque_ptr)};
    const auto table{builder_.emit_load(gir::value{table_at, opaque_ptr}, opaque_ptr)};
    const auto index{static_cast<u64>(slot - methods.begin())};
    const auto entry_at{builder_.emit_get_element_ptr(
        gir::value{table, opaque_ptr}, {usize_constant(index)}, opaque_ptr)};
    const auto entry{builder_.emit_load(gir::value{entry_at, opaque_ptr}, opaque_ptr)};

    values.emplace_back(data, opaque_ptr);
    for (usize i{0}; i < args.size(); ++i) {
        record_reader p{m, m.data_of(params[i + 1]).pl.extra};
        p.skip_str();
        auto&      param_type{*TRY(sig.eval_type(p.body_field(), m.src_of(params[i + 1])))};
        const auto arg{TRY(resolve_value(args[i], src))};
        const auto coerced{TRY(coerce_to(
            arg,
            param_type,
            src,
            {.what = use_site::kind::ARGUMENT, .arg_index = i + 1, .callee = slot->name}))};
        values.emplace_back(TRY(as_runtime(coerced, src)));
    }

    auto&      ret_type{*TRY(sig.eval_type(ret, m.src_of(slot->func)))};
    const auto result{
        builder_.emit_indirect_call(gir::value{entry, opaque_ptr}, std::move(values), ret_type)};
    if (!result) { return stdx::option<operand>{void_value()}; }
    return stdx::option<operand>{operand{gir::value{*result, ret_type}}};
}

} // namespace ghoti::uir::sema
