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
constexpr std::string_view ASSIGN_NOT_FOLDABLE{
    "`comptime let mut` assignment must be known at compile time"};
constexpr std::string_view STORE_UNDER_RUNTIME{
    "`comptime let mut` assignment depends on a runtime condition"};

} // namespace

auto analyzer::analyze_alloc_mut(inst_id inst, bool is_comptime_alloc) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    DISCARD(rec.str_field());
    const auto              type_ref{rec.ref_field()};
    stdx::option<gs::type&> type;
    if (type_ref) { type.emplace(*TRY(resolve_type(*type_ref, src))); }

    // Mutable state at compile time lives in a comptime allocation
    if (is_comptime_alloc || is_comptime()) {
        return comp_.new_alloc({
            .type                = type,
            .value               = stdx::none,
            .frame               = frame_,
            .runtime_depth       = runtime_depth_,
            .keeps_comptime_type = is_comptime_alloc,
        });
    }
    if (!type) {
        // The first store gives an untyped `let mut` its type
        return place{
            .address = {}, .pointee = &builtin(gs::type_kind::UNDEFINED), .is_mutable = true};
    }
    const auto slot{builder_.emit_alloca(*type)};
    return place{.address = gir::value{slot, *type}, .pointee = &*type, .is_mutable = true};
}

auto analyzer::store_comptime(comptime_place p, const operand& value, const source& src)
    -> result<void> {
    auto& alloc{comp_.alloc_at(p)};
    if (alloc.frame != frame_) {
        return comp_.unsupported(u_, src, "a store to another analysis's comptime allocation");
    }
    if (alloc.runtime_depth != runtime_depth_) {
        return comp_.report(
            u_, src, gs::error::COMPTIME_MUT_ASSIGN_NOT_FOLDABLE, std::string{STORE_UNDER_RUNTIME});
    }

    // An untyped `.{...}` or `.name` is known once it takes the slot's type
    if (is_untyped_literal(value)) {
        if (!alloc.type) {
            return comp_.unsupported(u_, src, "an untyped literal with no type to take");
        }
        auto& type{*alloc.type};
        return store_comptime(
            p, TRY(coerce_to(value, type, src, {.what = use_site::kind::STORE})), src);
    }
    DISCARD(TRY(require_comptime(value, src)));
    if (!alloc.type) {
        // An untyped `comptime let mut` stays comptime_int, an untyped `let mut` takes a type
        auto& from{type_of(value)};
        alloc.type.emplace(alloc.keeps_comptime_type ? from : type_of(TRY(concrete(value, src))));
    }
    const auto stored{TRY(coerce_to(value, *alloc.type, src, {.what = use_site::kind::STORE}))};
    // `alloc` may have moved while coercing
    comp_.alloc_at(p).value = *comptime_value(stored);
    comp_.note_store(p);
    return {};
}

auto analyzer::load_comptime(comptime_place p) -> operand {
    auto& alloc{comp_.alloc_at(p)};
    auto& type{alloc.type.value_or(builtin(gs::type_kind::UNDEFINED))};
    return alloc.value.value_or(comp_.constants().intern(ip::undefined_value{}, type));
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
    if (const auto index{comptime_value(init)}; index && comp_.constants().is_type(*index)) {
        return comp_.report(
            u_, src, gs::error::MUTABLE_TYPE_BINDING, std::string{messages::MUTABLE_TYPE_BINDING});
    }
    if (!type_ref && (init.is<anon_init>() || init.is<enum_literal>())) {
        return comp_.unsupported(u_, src, "an untyped literal bound with no type");
    }
    auto                    value{init};
    stdx::option<gs::type&> declared;
    if (type_ref) {
        declared.emplace(*TRY(resolve_type(*type_ref, src)));
        value = TRY(coerce_to(init, *declared, src, {.what = use_site::kind::STORE}));
    }
    if (is_comptime()) { return value; }

    // An aggregate binding keeps one address for every use, so it gets a slot of its own
    auto& type{declared && gs::is_structural(declared->get_kind()) ? *declared : type_of(value)};
    if (!gs::is_structural(type.get_kind()) || value.is<merge_ref>()) { return value; }

    // An `undefined` initializer leaves the fresh slot as it is
    const auto slot{builder_.emit_alloca(type)};
    const auto init_index{comptime_value(init)};
    if (!init_index || !comp_.constants().value_at(*init_index).data.is<ip::undefined_value>()) {
        builder_.emit_store(slot, TRY(as_runtime(value, src))).is_initializer = true;
    }
    return slot_value{.address = gir::value{slot, type}, .type = &type};
}

auto analyzer::analyze_const_local(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  name{rec.str_field()};
    const auto  type_ref{rec.ref_field()};
    const auto  body{rec.body_field()};
    const auto& init_src{body.empty() ? src : src_of(body.back())};

    auto value{[&] {
        const auto scope{comptime_scope()};
        return analyze_body(body);
    }()};
    if (!value && value.error() == stop::RUNTIME) {
        return comp_.report_runtime_need(
            u_, init_src, gs::error::COMPTIME_MUT_NOT_FOLDABLE, std::string{CONST_CONSTANT});
    }
    const auto out{TRY(std::move(value))};
    if (jump_) { return comp_.unsupported(u_, src, "a jump out of a `const` initializer"); }
    if (!out.yielded) { return comp_.unsupported(u_, src, "a `const` body that doesn't yield"); }
    if (out.yielded->is<function_ref>()) { return comp_.unsupported(u_, src, "a local function"); }
    const auto known{comptime_value(*out.yielded)};
    if (!known) {
        return comp_.report(
            u_, init_src, gs::error::COMPTIME_MUT_NOT_FOLDABLE, std::string{CONST_CONSTANT});
    }

    // A type declared here is named for the local, `Local` rather than `struct`
    if (comp_.constants().is_type(*known) && !body.empty() &&
        mod().tag_of(body.back()) == tag::yield) {
        auto&      type{comp_.constants().type_at(*known)};
        const auto n{comp_.nominal_of(type)};
        if (n && as_inst(mod().data_of(body.back()).un.operand) == n->inst) {
            comp_.name_nominal(type, name);
            TRY(comp_.require_fields(u_, src, type));
        }
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
    // `a[i..j] = v` copies into the range
    if (const auto lhs{as_inst(d.bin.lhs)}; lhs && mod().tag_of(*lhs) == tag::slice) {
        return comp_.unsupported(u_, src, "a copy into a slice range");
    }
    if (const auto comptime_slot{target.as_opt<comptime_place>()}) {
        // A runtime function's store into comptime memory runs its value at compile time
        auto known{[&] -> result<operand> {
            if (comptime_value(value)) { return value; }
            const auto scope{comptime_scope()};
            return reevaluate(d.bin.rhs, src);
        }()};
        if (!known && known.error() == stop::RUNTIME) {
            return comp_.report_runtime_need(u_,
                                             src,
                                             gs::error::COMPTIME_MUT_ASSIGN_NOT_FOLDABLE,
                                             std::string{ASSIGN_NOT_FOLDABLE});
        }
        if (!known) { return stdx::err{known.error()}; }
        TRY(store_comptime(*comptime_slot, *known, src));
        return void_value();
    }
    TRY(require_runtime(src));

    // A reference binding written through is spilled first, as the old emitter takes its place
    auto through{target};
    if (const auto lhs{as_inst(d.bin.lhs)};
        lhs && is_binding(mod().tag_of(*lhs)) && !target.is<place>() &&
        type_of(target).get_kind() == gs::type_kind::REFERENCE) {
        through = load(TRY(address_of(d.bin.lhs, src)));
    }
    auto destination{TRY(store_destination(through, src))};
    if (!destination.is_mutable) {
        return comp_.report(u_, src, gs::error::ASSIGNMENT_TO_CONST, std::string{ASSIGN_CONSTANT});
    }
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
    const auto known{comptime_value(value)};
    if (!known || !comp_.constants().value_at(*known).data.is<ip::undefined_value>()) {
        builder_.emit_store(destination.address, TRY(as_runtime(stored, src)));
    }
    return void_value();
}

auto analyzer::store_destination(const operand& target, const source& src) -> result<place> {
    if (const auto slot{target.as_opt<place>()}) { return slot->pending ? settle(*slot) : *slot; }
    if (const auto slot{target.as_opt<slot_value>()}; slot && slot->is_mutable) {
        return place{.address = slot->address, .pointee = slot->type, .is_mutable = true};
    }
    // `*p = v` stores through the pointer or reference `p`
    if (gs::pointee_of(type_of(target)) && !target.is<comptime_place>()) {
        return pointer_place(target, src);
    }
    return place{.address = {}, .pointee = &builtin(gs::type_kind::VOID_), .is_mutable = false};
}

auto analyzer::analyze_load(inst_id inst) -> result<operand> {
    const auto& src{src_of(inst)};
    const auto  target{TRY(resolve(mod().data_of(inst).un.operand, src))};
    if (const auto comptime_slot{target.as_opt<comptime_place>()}) {
        return load_comptime(*comptime_slot);
    }
    if (const auto slot{target.as_opt<slot_value>()}) {
        TRY(require_runtime(src));
        return operand{
            load(place{.address = slot->address, .pointee = slot->type, .is_mutable = false})};
    }
    // A field of a compile-time value was already read
    if (comptime_value(target)) { return target; }
    const auto slot{target.as_opt<place>()};
    if (!slot || (!slot->pending && !slot->address.type)) {
        return comp_.unsupported(u_, src, "a load from a non-place");
    }
    TRY(require_runtime(src));
    return operand{TRY(read(*slot, src))};
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
    return void_value();
}

} // namespace ghoti::uir::sema
