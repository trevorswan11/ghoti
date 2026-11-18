#include <string>
#include <string_view>
#include <utility>

#include <gsl/util>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/utility.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"

namespace ghoti::uir::sema {

namespace {

constexpr std::string_view CONST_CONSTANT{"`const` initializer must be known at compile time"};
constexpr std::string_view ASSIGN_CONSTANT{"Cannot assign to constant variable"};

} // namespace

auto analyzer::analyze_alloc_mut(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    DISCARD(rec.str_field());
    const auto type_ref{rec.ref_field()};
    TRY(require_runtime(src));
    if (!type_ref) {
        // The first store gives an untyped `let mut` its type
        return place{
            .address = {}, .pointee = &builtin(gs::type_kind::UNDEFINED), .is_mutable = true};
    }
    auto&      type{*TRY(resolve_type(*type_ref, src))};
    const auto slot{builder_.emit_alloca(type)};
    return place{.address = gir::value{slot, type}, .pointee = &type, .is_mutable = true};
}

auto analyzer::analyze_let_value(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    DISCARD(rec.str_field());
    const auto type_ref{rec.ref_field()};
    const auto init{TRY(resolve_value(*rec.ref_field(), src))};
    if (init.is<function_ref>()) {
        return comp_.unsupported(u_, src, "a function bound to a `let`");
    }
    if (!type_ref) { return init; }
    auto& type{*TRY(resolve_type(*type_ref, src))};
    return coerce_to(init, type, src, {.what = use_site::kind::STORE});
}

auto analyzer::analyze_const_local(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    DISCARD(rec.str_field());
    const auto  type_ref{rec.ref_field()};
    const auto  body{rec.body_field()};
    const auto& init_src{body.empty() ? src : src_of(body.back())};

    needed_runtime_ = false;
    ++comptime_depth_;
    auto value{[&] {
        const auto restore{gsl::finally([this] { --comptime_depth_; })};
        return analyze_body(body);
    }()};
    if (!value && needed_runtime_) {
        return comp_.report(
            u_, init_src, gs::error::COMPTIME_MUT_NOT_FOLDABLE, std::string{CONST_CONSTANT});
    }
    const auto out{TRY(std::move(value))};
    if (!out.yielded) { return comp_.unsupported(u_, src, "a `const` body that doesn't yield"); }
    if (out.yielded->is<function_ref>()) { return comp_.unsupported(u_, src, "a local function"); }
    if (!comptime_value(*out.yielded)) {
        return comp_.report(
            u_, init_src, gs::error::COMPTIME_MUT_NOT_FOLDABLE, std::string{CONST_CONSTANT});
    }
    if (!type_ref) { return *out.yielded; }
    auto& type{*TRY(resolve_type(*type_ref, src))};
    return coerce_to(*out.yielded, type, init_src, {.what = use_site::kind::STORE});
}

auto analyzer::analyze_store(inst_id inst) -> result<operand> {
    const auto& d{mod().data_of(inst)};
    const auto& src{src_of(inst)};
    const auto  target{TRY(resolve(d.bin.lhs, src))};
    const auto  value{TRY(resolve_value(d.bin.rhs, src))};
    TRY(require_runtime(src));
    const auto slot{target.as_opt<place>()};
    if (!slot || !slot->is_mutable) {
        return comp_.report(u_, src, gs::error::ASSIGNMENT_TO_CONST, std::string{ASSIGN_CONSTANT});
    }

    auto destination{*slot};
    if (!destination.address.type) {
        // An untyped `let mut` takes its initializer's concrete type
        const auto typed{TRY(concrete(value, src))};
        auto&      type{type_of(typed)};
        destination.address = gir::value{builder_.emit_alloca(type), type};
        destination.pointee = &type;
        values_.insert_or_assign(*as_inst(d.bin.lhs), operand{destination});
    }
    const auto stored{
        TRY(coerce_to(value, *destination.pointee, src, {.what = use_site::kind::STORE}))};
    // Storing `undefined` leaves the slot as it is, so nothing is emitted
    if (type_of(value).get_kind() != gs::type_kind::UNDEFINED) {
        builder_.emit_store(destination.address.local(), TRY(as_runtime(stored, src)));
    }
    return comp_.constants().intern(ip::void_value{}, builtin(gs::type_kind::VOID_));
}

auto analyzer::analyze_load(inst_id inst) -> result<operand> {
    const auto& src{src_of(inst)};
    const auto  target{TRY(resolve(mod().data_of(inst).un.operand, src))};
    const auto  slot{target.as_opt<place>()};
    if (!slot || !slot->address.type) {
        return comp_.unsupported(u_, src, "a load from a non-place");
    }
    TRY(require_runtime(src));
    return gir::value{builder_.emit_load(slot->address.local(), *slot->pointee), *slot->pointee};
}

auto analyzer::analyze_decl_val(inst_id inst, bool as_place) -> result<operand> {
    const auto&   src{src_of(inst)};
    const decl_id decl{mod().data_of(inst).un.aux};
    const auto&   d{mod().decl_at(decl)};
    if (d.kind != decl_kind::VALUE) {
        return comp_.unsupported(u_, src, "a use of an import or other non-value declaration");
    }
    const auto value{TRY(comp_.analyze_decl(u_, decl, src))};
    const auto global{value.as_opt<global_ref>()};
    if (!global) {
        if (as_place) {
            return comp_.unsupported(u_, src, "a place on a non-`let mut` declaration");
        }
        return value;
    }
    // Reading a `let mut` at compile time is either an error or only its type, as in `@TypeOf`
    if (is_comptime()) {
        return comp_.unsupported(u_, src, "a compile-time use of a `let mut` global");
    }
    auto&       type{*global->type};
    const auto  address{builder_.emit_global_addr(*global->symbol, type, false)};
    const place slot{.address = gir::value{address, type}, .pointee = &type, .is_mutable = true};
    if (as_place) { return slot; }
    return gir::value{builder_.emit_load(address, type), type};
}

auto analyzer::analyze_discard(inst_id inst) -> result<operand> {
    const auto& src{src_of(inst)};
    const auto  value{TRY(resolve_value(mod().data_of(inst).un.operand, src))};
    if (value.is<merge_ref>()) { DISCARD(TRY(concrete(value, src))); }
    return comp_.constants().intern(ip::void_value{}, builtin(gs::type_kind::VOID_));
}

} // namespace ghoti::uir::sema
