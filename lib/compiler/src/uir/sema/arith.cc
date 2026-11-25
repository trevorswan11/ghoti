#include <array>
#include <compare>
#include <vector>

#include <fmt/format.h>
#include <gsl/util>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/gir/semantics.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/big_int.hh"
#include "support/float128.hh"
#include "support/int128.hh"

namespace ghoti::uir::sema {

namespace semantics = gir::semantics;
using gir::instruction_kind;

namespace {

constexpr u64 MAX_COMPTIME_INT_BITS{u64{1} << 24};

[[nodiscard]] constexpr auto binary_info_of(tag t) noexcept -> binary_info {
    using enum op_class;
    switch (t) {
    case tag::add:      return {instruction_kind::ADD, ARITHMETIC};
    case tag::sub:      return {instruction_kind::SUB, ARITHMETIC};
    case tag::mul:      return {instruction_kind::MUL, ARITHMETIC};
    case tag::div:      return {instruction_kind::DIV, ARITHMETIC};
    case tag::mod:      return {instruction_kind::MOD, ARITHMETIC};
    case tag::add_wrap: return {instruction_kind::ADD, ARITHMETIC, true};
    case tag::sub_wrap: return {instruction_kind::SUB, ARITHMETIC, true};
    case tag::mul_wrap: return {instruction_kind::MUL, ARITHMETIC, true};
    case tag::add_sat:  return {instruction_kind::ADD, ARITHMETIC, false, true};
    case tag::sub_sat:  return {instruction_kind::SUB, ARITHMETIC, false, true};
    case tag::mul_sat:  return {instruction_kind::MUL, ARITHMETIC, false, true};
    case tag::shl:      return {instruction_kind::SHL, SHIFT};
    case tag::shl_wrap: return {instruction_kind::SHL, SHIFT, true};
    case tag::shl_sat:  return {instruction_kind::SHL, SHIFT, false, true};
    case tag::shr:      return {instruction_kind::SHR, SHIFT};
    case tag::bit_and:  return {instruction_kind::AND, BITWISE};
    case tag::bit_or:   return {instruction_kind::OR, BITWISE};
    case tag::bit_xor:  return {instruction_kind::XOR, BITWISE};
    case tag::cmp_eq:   return {instruction_kind::EQ, EQUALITY};
    case tag::cmp_ne:   return {instruction_kind::NE, EQUALITY};
    case tag::cmp_lt:   return {instruction_kind::LT, ORDERING};
    case tag::cmp_le:   return {instruction_kind::LE, ORDERING};
    case tag::cmp_gt:   return {instruction_kind::GT, ORDERING};
    case tag::cmp_ge:   return {instruction_kind::GE, ORDERING};
    default:            UNREACHABLE("not a binary operator");
    }
}

// An operator that computes in its operands' type, which an expected type can widen
[[nodiscard]] constexpr auto is_widenable_operator(tag t) noexcept -> bool {
    switch (t) {
    case tag::add:
    case tag::sub:
    case tag::mul:
    case tag::div:
    case tag::mod:
    case tag::add_wrap:
    case tag::sub_wrap:
    case tag::mul_wrap:
    case tag::add_sat:
    case tag::sub_sat:
    case tag::mul_sat:
    case tag::bit_and:
    case tag::bit_or:
    case tag::bit_xor:  return true;
    default:            return false;
    }
}

[[nodiscard]] auto compare_op_of(instruction_kind kind) noexcept -> semantics::int_compare_op {
    switch (kind) {
    case instruction_kind::EQ: return semantics::int_compare_op::EQ;
    case instruction_kind::NE: return semantics::int_compare_op::NE;
    case instruction_kind::LT: return semantics::int_compare_op::LT;
    case instruction_kind::LE: return semantics::int_compare_op::LE;
    case instruction_kind::GT: return semantics::int_compare_op::GT;
    default:                   return semantics::int_compare_op::GE;
    }
}

// Whether the operator applies to operands of these types, before looking for their peer
[[nodiscard]] auto applies(op_class cls, const gs::type& lhs, const gs::type& rhs) noexcept
    -> bool {
    const auto numeric{gs::is_numeric(lhs.get_kind()) && gs::is_numeric(rhs.get_kind())};
    switch (cls) {
    case op_class::ARITHMETIC:
    case op_class::ORDERING:   return numeric;
    case op_class::BITWISE:
    case op_class::SHIFT:      return is_int_like(lhs) && is_int_like(rhs);
    case op_class::EQUALITY:
        return numeric || (is_bool(lhs) && is_bool(rhs)) ||
               (lhs.get_kind() == gs::type_kind::ENUM && gs::is_same_unqualified(lhs, rhs));
    }

    return false;
}

[[nodiscard]] auto compare(semantics::int_compare_op op, const big_int& lhs, const big_int& rhs)
    -> bool {
    switch (op) {
    case semantics::int_compare_op::EQ: return lhs == rhs;
    case semantics::int_compare_op::NE: return lhs != rhs;
    case semantics::int_compare_op::LT: return lhs < rhs;
    case semantics::int_compare_op::LE: return lhs <= rhs;
    case semantics::int_compare_op::GT: return lhs > rhs;
    case semantics::int_compare_op::GE: return lhs >= rhs;
    }

    return false;
}

} // namespace

auto analyzer::folds_exactly(const gs::type& t) const -> bool {
    return t.get_kind() != gs::type_kind::INT || gs::int_width(t) <= 128;
}

auto analyzer::domain_of(const gs::type& t) const -> semantics::int_domain {
    const auto bits{t.get_kind() == gs::type_kind::INT ? gs::int_width(t)
                                                       : static_cast<u16>(comp_.ptr_bits())};
    return {.bits = bits, .is_signed = gs::is_signed_integer(t)};
}

auto analyzer::checks_overflow(semantics::int_op op, const gs::type& t) const -> bool {
    return runtime_safety() && gs::is_integer(t.get_kind()) &&
           semantics::any(
               semantics::runtime_checks(op, {.bits = 1, .is_signed = gs::is_signed_integer(t)}));
}

auto analyzer::analyze_binary(inst_id inst, tag t) -> result<operand> {
    const auto& d{mod().data_of(inst)};
    const auto& src{src_of(inst)};
    auto        lhs{TRY(resolve_value(d.bin.lhs, src))};

    // A compound assignment stores through the reference its left side was read through. A
    // union's tag is compared through the reference
    const auto reads_through{[&](const operand& op) {
        auto& type{type_of(op)};
        return type.get_kind() == gs::type_kind::REFERENCE && !is_comptime() &&
               !gs::pointee_of(type)->get_data().is<gs::types::union_t>();
    }};
    if (reads_through(lhs)) {
        const auto address{TRY(as_runtime(lhs, src))};
        reference_reads_.insert_or_assign(inst, address);
        lhs = TRY(read_through_reference(address, src));
    }

    auto rhs{TRY(resolve_value(d.bin.rhs, src))};
    if (reference_reads_.contains(inst)) {
        const auto made{as_inst(d.bin.lhs).and_then([&](inst_id i) {
            const auto found{reference_segments_.find(i)};
            return found == reference_segments_.end()
                       ? stdx::option<gir::segment_id>{}
                       : stdx::option<gir::segment_id>{found->second};
        })};
        if (made && *made != builder_.get_segment()->get_id()) {
            return comp_.unsupported(u_, src, "a reference read after a right side that branches");
        }
    }
    if (reads_through(rhs)) { rhs = TRY(read_through_reference(rhs, src)); }

    // A compile-time reference is read for what it refers to
    for (auto* side : {&lhs, &rhs}) {
        if (type_of(*side).get_kind() != gs::type_kind::REFERENCE ||
            (!memory_place(*side) && !is_comptime())) {
            continue;
        }

        if (const auto referent{comptime_referent(*side)}) { *side = operand{*referent}; }
    }

    // An enum literal names a variant of the other side's enum
    if (lhs.is<enum_literal>() && type_of(rhs).get_kind() == gs::type_kind::ENUM) {
        lhs = TRY(coerce_to(lhs, type_of(rhs), src, {.what = use_site::kind::STORE}));
    } else if (rhs.is<enum_literal>() && type_of(lhs).get_kind() == gs::type_kind::ENUM) {
        rhs = TRY(coerce_to(rhs, type_of(lhs), src, {.what = use_site::kind::STORE}));
    }

    const auto info{binary_info_of(t)};
    if (const auto known{compare_out_of_range(info, lhs, rhs)}) {
        return operand{comp_.constants().intern(*known, builtin(gs::type_kind::BOOL))};
    }

    if (info.cls == op_class::EQUALITY) {
        // Types are interned, so two are equal when they are the same type
        const auto lhs_known{comptime_value(lhs)};
        const auto rhs_known{comptime_value(rhs)};
        auto&      pool{comp_.constants()};
        if (lhs_known && rhs_known && pool.is_type(*lhs_known) && pool.is_type(*rhs_known)) {
            const bool same{&pool.type_at(*lhs_known) == &pool.type_at(*rhs_known)};
            return operand{
                pool.intern(t == tag::cmp_eq ? same : !same, builtin(gs::type_kind::BOOL))};
        }

        if (const auto compared{TRY(compare_tag(info, lhs, rhs, src))}) { return *compared; }
        if (const auto compared{TRY(compare_pointers(info, lhs, rhs, src))}) { return *compared; }
    }

    // A range capture takes the other side's integer type when that holds every value it takes
    const auto narrows_to{[&](const operand& capture, const operand& other) {
        const auto slot{capture.as_opt<slot_value>()};
        auto&      target{type_of(other)};
        return slot && slot->range && gs::is_integer(target.get_kind()) &&
               !gs::is_same_unqualified(*slot->type, target) &&
               gs::comptime_int_fits(slot->range->lo, target, comp_.ptr_bits()) &&
               gs::comptime_int_fits(slot->range->hi, target, comp_.ptr_bits());
    }};
    // A one-argument cast takes the type of the side it meets
    const auto cast_meets{[&](const operand& cast, const operand& other) {
        return info.cls != op_class::SHIFT && cast.is<cast_ref>() && !other.is<cast_ref>() &&
               !is_untyped_literal(other) && !gs::is_comptime_numeric(type_of(other).get_kind());
    }};
    // An inferred cast as a shift amount takes the shifted value's type
    if (info.cls == op_class::SHIFT && rhs.is<cast_ref>() && !lhs.is<cast_ref>() &&
        !is_untyped_literal(lhs) && !gs::is_comptime_numeric(type_of(lhs).get_kind())) {
        rhs = TRY(coerce_to(rhs, type_of(lhs), src, {.what = use_site::kind::STORE}));
    }

    if (cast_meets(lhs, rhs)) {
        lhs = TRY(coerce_to(lhs, type_of(rhs), src, {.what = use_site::kind::STORE}));
    } else if (cast_meets(rhs, lhs)) {
        rhs = TRY(coerce_to(rhs, type_of(lhs), src, {.what = use_site::kind::STORE}));
    } else if (info.cls != op_class::SHIFT && (lhs.is<cast_ref>() || rhs.is<cast_ref>())) {
        // Arithmetic of an inferred cast and an untyped value waits for the type it is used as
        if (info.cls == op_class::EQUALITY || info.cls == op_class::ORDERING) {
            return comp_.unsupported(u_, src, "an inferred cast meeting an untyped value");
        }

        return cast_ref{inst};
    }

    if (narrows_to(lhs, rhs)) {
        lhs = TRY(coerce_to(lhs, type_of(rhs), src, {.what = use_site::kind::STORE}));
    } else if (narrows_to(rhs, lhs)) {
        rhs = TRY(coerce_to(rhs, type_of(lhs), src, {.what = use_site::kind::STORE}));
    }

    auto& lhs_type{type_of(lhs)};
    auto& rhs_type{type_of(rhs)};
    if (lhs_type.is_poison() || rhs_type.is_poison()) { return stdx::err{stop::POISON}; }

    // A vector side applies the operator lane by lane
    if (vector_read(lhs_type) || vector_read(rhs_type)) {
        return vector_binary(info, lhs, rhs, src);
    }

    // Two uncoerced `.name`s are equal when their names are
    const auto lhs_literal{lhs.as_opt<enum_literal>()};
    const auto rhs_literal{rhs.as_opt<enum_literal>()};
    if (info.cls == op_class::EQUALITY && lhs_literal && rhs_literal) {
        const bool same{literal_name(*lhs_literal) == literal_name(*rhs_literal)};
        return operand{comp_.constants().intern(info.kind == instruction_kind::EQ ? same : !same,
                                                builtin(gs::type_kind::BOOL))};
    }

    // `void` has a single value, so two of them are always equal
    if (info.cls == op_class::EQUALITY && lhs_type.get_kind() == gs::type_kind::VOID_ &&
        rhs_type.get_kind() == gs::type_kind::VOID_) {
        return operand{comp_.constants().intern(info.kind == instruction_kind::EQ,
                                                builtin(gs::type_kind::BOOL))};
    }

    // `ptr + n`, `n + ptr` and `ptr - n` step by whole pointees, the pointer written first
    const bool lhs_ptr{lhs_type.get_kind() == gs::type_kind::POINTER};
    const bool rhs_ptr{rhs_type.get_kind() == gs::type_kind::POINTER};
    const bool offsets{(info.kind == instruction_kind::ADD && lhs_ptr != rhs_ptr) ||
                       (info.kind == instruction_kind::SUB && lhs_ptr && !rhs_ptr)};
    if (offsets && !info.wrapping && !info.saturating) {
        const auto& ptr{lhs_ptr ? lhs : rhs};
        const auto& count{lhs_ptr ? rhs : lhs};
        if (is_int_like(type_of(count))) {
            TRY(require_runtime(src));
            const auto at{TRY(as_runtime(ptr, src))};
            const auto typed{gs::is_comptime_numeric(type_of(count).get_kind())
                                 ? TRY(coerce_to(count,
                                                 builtin(gs::type_kind::USIZE),
                                                 src,
                                                 {.what = use_site::kind::STORE}))
                                 : count};
            const auto by{TRY(as_runtime(typed, src))};
            auto&      ptr_type{*at.type};
            return gir::value{builder_.emit_binary(info.kind, at, by, ptr_type), ptr_type};
        }
    }

    // Operators on strings, arrays and other aggregates arrive with their own phases
    const auto plain{[](const gs::type& type) {
        return gs::is_numeric(type.get_kind()) || type.get_kind() == gs::type_kind::BOOL ||
               type.get_kind() == gs::type_kind::ENUM;
    }};
    if (!plain(lhs_type) || !plain(rhs_type)) {
        return comp_.unsupported(u_, src, "an operator on this kind of value");
    }

    if (!applies(info.cls, lhs_type, rhs_type)) {
        return comp_.report(
            u_,
            src,
            gs::error::OPERATOR_TYPE_MISMATCH,
            messages::operator_mismatch(comp_.ctx(), operator_spelling(t), lhs_type, rhs_type));
    }

    if (info.cls == op_class::SHIFT) { return analyze_shift(inst, info, lhs, rhs); }

    if (const auto merged{TRY(fold_into_merge(info, lhs, rhs, src))}) { return *merged; }

    const std::array operands{lhs, rhs};
    auto&            peer{*TRY(peer_type(operands, src))};

    // Untyped operands only stay untyped when both are known
    const bool both_known{comptime_value(lhs) && comptime_value(rhs)};
    auto&      work{both_known || !gs::is_comptime_numeric(peer.get_kind())
                        ? peer
                        : comp_.ctx().default_concrete(peer)};
    const auto l{
        TRY(coerce_to(lhs, work, src, {.what = use_site::kind::STORE, .from = d.bin.lhs}))};
    const auto r{
        TRY(coerce_to(rhs, work, src, {.what = use_site::kind::STORE, .from = d.bin.rhs}))};

    if (const auto folded{TRY(fold_binary(info, work, l, r, src))}) { return *folded; }
    TRY(require_runtime(src));
    const bool is_comparison{info.cls == op_class::EQUALITY || info.cls == op_class::ORDERING};
    const auto lv{TRY(as_runtime(l, src))};
    const auto rv{TRY(as_runtime(r, src))};
    if (is_comparison) {
        auto& result_type{builtin(gs::type_kind::BOOL)};
        return gir::value{builder_.emit_binary(info.kind, lv, rv, result_type), result_type};
    }

    auto&      result_type{type_of(operand{lv})};
    const auto op{semantics::int_op_of(info.kind, info.wrapping, info.saturating)};
    const bool checked{op && checks_overflow(*op, result_type)};
    return gir::value{
        builder_.emit_binary(info.kind, lv, rv, result_type, checked, info.saturating),
        result_type};
}

auto analyzer::compare_out_of_range(const binary_info& info, const operand& lhs, const operand& rhs)
    -> stdx::option<bool> {
    if (info.cls != op_class::EQUALITY && info.cls != op_class::ORDERING) { return stdx::none; }

    const auto outside{[&](const operand& typed, const operand& untyped) -> stdx::option<bool> {
        auto&      type{type_of(typed)};
        const auto known{comptime_value(untyped)};
        if (!gs::is_integer(type.get_kind()) || !known ||
            !gs::is_comptime_numeric(type_of(untyped).get_kind())) {
            return stdx::none;
        }

        const auto exact{int_of(comp_.constants().value_at(*known))};
        if (!exact || gs::comptime_int_fits(*exact, type, comp_.ptr_bits())) { return stdx::none; }
        return *exact > big_int{i128{0}};
    }};
    const auto order{[&](gir::instruction_kind kind, bool above) {
        switch (kind) {
        case instruction_kind::EQ: return false;
        case instruction_kind::NE: return true;
        case instruction_kind::LT:
        case instruction_kind::LE: return above;
        default:                   return !above;
        }
    }};
    const auto mirrored{[](gir::instruction_kind kind) {
        switch (kind) {
        case instruction_kind::LT: return instruction_kind::GT;
        case instruction_kind::LE: return instruction_kind::GE;
        case instruction_kind::GT: return instruction_kind::LT;
        case instruction_kind::GE: return instruction_kind::LE;
        default:                   return kind;
        }
    }};
    if (const auto above{outside(lhs, rhs)}) { return order(info.kind, *above); }
    if (const auto above{outside(rhs, lhs)}) { return order(mirrored(info.kind), *above); }
    return stdx::none;
}

auto analyzer::binary_as(inst_id inst, gs::type& target, const source& src) -> result<operand> {
    // Both sides take the target, then the operator applies to them where they were analyzed
    // A shift's amount keeps its own type
    const auto& d{mod().data_of(inst)};
    const bool  shift{binary_info_of(mod().tag_of(inst)).cls == op_class::SHIFT};
    std::vector<std::pair<inst_id, operand>> saved;
    const auto                               restore{gsl::finally([&] {
        for (auto& [i, value] : saved) { values_.insert_or_assign(i, std::move(value)); }
    })};
    for (const auto side : {d.bin.lhs, d.bin.rhs}) {
        if (shift && side == d.bin.rhs) { continue; }
        const auto side_inst{as_inst(side)};
        if (!side_inst) { continue; }
        const auto found{values_.find(*side_inst)};
        if (found == values_.end()) { continue; }
        const auto typed{
            TRY(coerce_to(found->second, target, src, {.what = use_site::kind::STORE}))};
        saved.emplace_back(*side_inst, found->second);
        values_.insert_or_assign(*side_inst, typed);
    }

    return analyze_binary(inst, mod().tag_of(inst));
}

auto analyzer::widened_arithmetic(ref r, gs::type& target, const source& src)
    -> result<stdx::option<gir::value>> {
    const auto inst{as_inst(r)};
    if (!inst || !is_widenable_operator(mod().tag_of(*inst)) || is_comptime()) {
        return stdx::none;
    }

    const auto& d{mod().data_of(*inst)};
    const auto  lhs{TRY(resolve(d.bin.lhs, src))};
    const auto  rhs{TRY(resolve(d.bin.rhs, src))};
    const auto  untyped{[&](const operand& op) {
        return comptime_value(op) && gs::is_comptime_numeric(type_of(op).get_kind());
    }};
    if (untyped(lhs) == untyped(rhs)) { return stdx::none; }

    // A builtin's untyped result, like `@sizeOf`'s, meets the typed side at its type
    const auto untyped_inst{as_inst(untyped(lhs) ? d.bin.lhs : d.bin.rhs)};
    if (untyped_inst && mod().tag_of(*untyped_inst) == tag::builtin) { return stdx::none; }

    const auto widened{[&](const operand& op) -> result<gir::value> {
        if (untyped(op)) {
            return as_runtime(TRY(coerce_to(op, target, src, {.what = use_site::kind::STORE})),
                              src);
        }

        const auto value{TRY(as_runtime(op, src))};
        if (!value.type || !gs::is_integer(value.type->get_kind()) ||
            !gs::is_implicit_widenable(*value.type, target)) {
            return comp_.unsupported(u_, src, "widening this operand");
        }

        return gir::value{builder_.emit_cast(gir::instruction_kind::WIDEN_CAST, value, target),
                          target};
    }};
    auto&      typed_type{untyped(lhs) ? type_of(rhs) : type_of(lhs)};
    if (!gs::is_integer(typed_type.get_kind()) || gs::is_same_unqualified(typed_type, target) ||
        !gs::is_implicit_widenable(typed_type, target)) {
        return stdx::none;
    }

    const auto lv{TRY(widened(lhs))};
    const auto rv{TRY(widened(rhs))};
    const auto info{binary_info_of(mod().tag_of(*inst))};
    const auto op{semantics::int_op_of(info.kind, info.wrapping, info.saturating)};
    const bool checked{op && checks_overflow(*op, target)};
    return stdx::option<gir::value>{gir::value{
        builder_.emit_binary(info.kind, lv, rv, target, checked, info.saturating), target}};
}

auto analyzer::fold_into_merge(const binary_info& info,
                               const operand&     lhs,
                               const operand&     rhs,
                               const source&      src) -> result<stdx::option<operand>> {
    // An untyped merge meeting a known untyped value stays untyped
    if (info.cls == op_class::EQUALITY || info.cls == op_class::ORDERING ||
        info.cls == op_class::SHIFT) {
        return stdx::none;
    }

    const bool  merge_left{lhs.is<merge_ref>()};
    const auto  m{merge_left ? lhs.as_opt<merge_ref>() : rhs.as_opt<merge_ref>()};
    const auto& other{merge_left ? rhs : lhs};
    if (!m || other.is<merge_ref>() || !comptime_value(other) ||
        !gs::is_comptime_numeric(type_of(other).get_kind())) {
        return stdx::none;
    }

    auto pending{merges_[m->index]};
    if (is_untyped_literal(pending.arrivals.front().value)) { return stdx::none; }
    TRY(require_runtime(src));
    pending.materialized.reset();
    pending.ops.emplace_back<merge_op>({.info = info, .other = other, .merge_left = merge_left});
    merges_.emplace_back(std::move(pending));
    return stdx::option<operand>{merge_ref{static_cast<u32>(merges_.size() - 1)}};
}

auto analyzer::compare_tag(const binary_info& info,
                           const operand&     lhs,
                           const operand&     rhs,
                           const source&      src) -> result<stdx::option<operand>> {
    // A tagged union compares its active field against an enum literal
    const bool literal_right{rhs.is<enum_literal>()};
    if (!literal_right && !lhs.is<enum_literal>()) { return stdx::none; }
    const auto& tagged{literal_right ? lhs : rhs};
    const auto  literal{(literal_right ? rhs : lhs).as<enum_literal>()};
    auto&       tagged_type{type_of(tagged)};
    const auto  referent{gs::pointee_of(tagged_type)};
    auto& union_type{referent && tagged_type.get_kind() == gs::type_kind::REFERENCE ? *referent
                                                                                    : tagged_type};
    if (union_type.get_kind() != gs::type_kind::UNION) { return stdx::none; }
    TRY(comp_.require_fields(u_, src, union_type));
    const auto ut{union_type.get_data().as_opt<gs::types::union_t>()};
    if (!ut || ut->is_untagged) { return stdx::none; }
    const auto name{literal_name(literal)};
    const auto index{TRY(field_index(union_type, name, src))};
    auto&      bool_type{builtin(gs::type_kind::BOOL)};
    auto&      pool{comp_.constants()};

    if (const auto known{comptime_value(tagged)}) {
        const auto un{pool.value_at(*known).data.as_opt<ip::union_value>()};
        if (!un) { return comp_.unsupported(u_, src, "comparing this comptime union"); }
        const bool equal{un->active_field == name};
        return operand{pool.intern(info.kind == instruction_kind::EQ ? equal : !equal, bool_type)};
    }

    TRY(require_runtime(src));

    // A reference already is the union's address
    gir::value base;
    if (&union_type == &tagged_type) {
        base = TRY(operand_address(tagged, src)).address;
    } else {
        base = gir::value{TRY(as_runtime(tagged, src)).data, union_type};
    }

    auto&      i32_type{comp_.ctx().get_int(32, true)};
    const auto tag_at{
        builder_.emit_get_element_ptr(base, {usize_constant(UNION_TAG_FIELD)}, i32_type)};
    const auto       tag{builder_.emit_load(gir::value{tag_at, i32_type}, i32_type)};
    const gir::value equal{
        builder_.emit_binary(instruction_kind::EQ,
                             gir::value{tag, i32_type},
                             constant(pool.intern(static_cast<i64>(index), i32_type), i32_type),
                             bool_type),
        bool_type};
    if (info.kind == instruction_kind::EQ) { return operand{equal}; }
    return operand{
        gir::value{builder_.emit_unary(instruction_kind::NOT, equal, bool_type), bool_type}};
}

auto analyzer::compare_pointers(const binary_info& info,
                                const operand&     lhs,
                                const operand&     rhs,
                                const source&      src) -> result<stdx::option<operand>> {
    const auto pointer_like{[&](const operand& op) {
        const auto kind{type_of(op).get_kind()};
        return kind == gs::type_kind::POINTER || kind == gs::type_kind::NULLPTR;
    }};
    if (!pointer_like(lhs) || !pointer_like(rhs)) { return stdx::none; }

    // Known pointers are equal when they are the same address or both null
    const auto known_l{comptime_value(lhs)};
    const auto known_r{comptime_value(rhs)};
    if (is_comptime() && !gir_fn_ && known_l && known_r &&
        (info.kind == instruction_kind::EQ || info.kind == instruction_kind::NE)) {
        auto&       pool{comp_.constants()};
        const auto& ld{pool.value_at(*known_l).data};
        const auto& rd{pool.value_at(*known_r).data};
        const bool  l_null{ld.is<ip::nullptr_value>()};
        const bool  r_null{rd.is<ip::nullptr_value>()};
        const bool  addresses{(ld.is<ip::address_value>() && rd.is<ip::address_value>()) ||
                              (ld.is<ip::memory_value>() && rd.is<ip::memory_value>())};
        if (l_null || r_null || addresses) {
            const bool equal{l_null || r_null ? l_null == r_null : ld == rd};
            return operand{pool.intern(equal == (info.kind == instruction_kind::EQ),
                                       builtin(gs::type_kind::BOOL))};
        }
    }

    // A runtime body folding a condition compares them at runtime instead
    if (is_comptime() && gir_fn_) { return comp_.needs_runtime(u_, src); }
    if (is_comptime()) { return comp_.unsupported(u_, src, "comparing pointers at compile time"); }
    // Even two known pointers compare at runtime
    auto&      bool_type{builtin(gs::type_kind::BOOL)};
    const auto l{TRY(as_runtime(lhs, src))};
    const auto r{TRY(as_runtime(rhs, src))};
    return operand{gir::value{builder_.emit_binary(info.kind, l, r, bool_type), bool_type}};
}

auto analyzer::fold_binary(const binary_info& info,
                           gs::type&          peer,
                           const operand&     lhs,
                           const operand&     rhs,
                           const source&      src) -> result<stdx::option<operand>> {
    const auto li{comptime_value(lhs)};
    const auto ri{comptime_value(rhs)};
    if (!li || !ri) { return stdx::none; }
    auto&       pool{comp_.constants()};
    const auto& lv{pool.value_at(*li)};
    const auto& rv{pool.value_at(*ri)};
    auto&       bool_type{builtin(gs::type_kind::BOOL)};
    const bool  is_comparison{info.cls == op_class::EQUALITY || info.cls == op_class::ORDERING};

    if (const auto lb{lv.data.as_opt<bool>()}) {
        const auto rb{rv.data.as_opt<bool>()};
        if (!rb) { return stdx::none; }
        switch (info.kind) {
        case instruction_kind::AND: return operand{pool.intern(*lb && *rb, bool_type)};
        case instruction_kind::OR:  return operand{pool.intern(*lb || *rb, bool_type)};
        case instruction_kind::XOR:
        case instruction_kind::NE:  return operand{pool.intern(*lb != *rb, bool_type)};
        case instruction_kind::EQ:  return operand{pool.intern(*lb == *rb, bool_type)};
        default:                    return stdx::none;
        }
    }

    if (const auto lf{lv.data.as_opt<f128>()}) {
        const auto rf{rv.data.as_opt<f128>()};
        if (!rf) { return stdx::none; }
        return fold_float(info, peer, *lf, *rf);
    }

    const auto l{int_of(lv)};
    const auto r{int_of(rv)};
    if (!l || !r) { return stdx::none; }
    if (is_comparison) {
        return operand{pool.intern(compare(compare_op_of(info.kind), *l, *r), bool_type)};
    }

    // An untyped value has no range to wrap or clamp to, so those operators are the plain ones
    if (peer.get_kind() == gs::type_kind::COMPTIME_INT) {
        const auto value{[&] -> stdx::option<big_int> {
            switch (info.kind) {
            case instruction_kind::ADD: return *l + *r;
            case instruction_kind::SUB: return *l - *r;
            case instruction_kind::MUL: return *l * *r;
            case instruction_kind::AND: return *l & *r;
            case instruction_kind::OR:  return *l | *r;
            case instruction_kind::XOR: return *l ^ *r;
            case instruction_kind::DIV:
            case instruction_kind::MOD:
                if (r->is_zero()) { return stdx::none; }
                return info.kind == instruction_kind::DIV ? *l / *r : *l % *r;
            default: return stdx::none;
            }
        }()};
        if (!value) {
            const auto fault{info.kind == instruction_kind::DIV
                                 ? semantics::int_fault::DIVISION_BY_ZERO
                                 : semantics::int_fault::MODULO_BY_ZERO};
            return comp_.report(
                u_,
                src,
                gs::error::COMPTIME_EVALUATION_FAILED,
                semantics::describe(fault, {}, comp_.ctx().type_display_name(peer)));
        }

        return operand{intern_int(pool, *value, peer)};
    }

    // A typed fold that faults is left to the runtime instruction, which checks it, unless
    // there is no runtime to leave it to
    const auto op{semantics::int_op_of(info.kind, info.wrapping, info.saturating)};
    if (!op) { return stdx::none; }
    const auto domain{domain_of(peer)};
    if (!folds_exactly(peer)) {
        const auto folded{semantics::fold_wide_int(*op, domain, *l, *r)};
        if (folded) { return operand{intern_int(pool, *folded, peer)}; }
        if (!is_comptime()) { return stdx::none; }
        return comp_.report(
            u_,
            src,
            gs::error::COMPTIME_EVALUATION_FAILED,
            semantics::describe(folded.error(), domain, comp_.ctx().type_display_name(peer)));
    }

    const auto bits{[&](const big_int& v) {
        return semantics::to_domain(
            v.is_negative() ? static_cast<u128>(*v.to_i128()) : *v.to_u128(), domain);
    }};
    const auto folded{semantics::fold_int(*op, domain, bits(*l), bits(*r))};
    if (!folded && is_comptime()) {
        return comp_.report(
            u_,
            src,
            gs::error::COMPTIME_EVALUATION_FAILED,
            semantics::describe(folded.error(), domain, comp_.ctx().type_display_name(peer)));
    }

    if (!folded) { return stdx::none; }
    const auto value{domain.is_signed ? big_int{semantics::signed_value(*folded, domain)}
                                      : big_int{semantics::unsigned_value(*folded, domain)}};
    return operand{intern_int(pool, value, peer)};
}

auto analyzer::fold_float(const binary_info& info, gs::type& peer, f128 lhs, f128 rhs)
    -> stdx::option<operand> {
    auto& pool{comp_.constants()};
    if (info.cls == op_class::EQUALITY || info.cls == op_class::ORDERING) {
        const auto order{lhs <=> rhs};
        const bool truth{[&] {
            switch (info.kind) {
            case instruction_kind::EQ: return order == std::partial_ordering::equivalent;
            case instruction_kind::NE: return order != std::partial_ordering::equivalent;
            case instruction_kind::LT: return order == std::partial_ordering::less;
            case instruction_kind::LE:
                return order == std::partial_ordering::less ||
                       order == std::partial_ordering::equivalent;
            case instruction_kind::GT: return order == std::partial_ordering::greater;
            default:
                return order == std::partial_ordering::greater ||
                       order == std::partial_ordering::equivalent;
            }
        }()};
        return operand{pool.intern(truth, builtin(gs::type_kind::BOOL))};
    }

    const auto op{[&] -> stdx::option<semantics::float_op> {
        switch (info.kind) {
        case instruction_kind::ADD: return semantics::float_op::ADD;
        case instruction_kind::SUB: return semantics::float_op::SUB;
        case instruction_kind::MUL: return semantics::float_op::MUL;
        case instruction_kind::DIV: return semantics::float_op::DIV;
        case instruction_kind::MOD: return semantics::float_op::REM;
        default:                    return stdx::none;
        }
    }()};
    if (!op || info.wrapping || info.saturating) { return stdx::none; }
    const auto format{gs::float_format_of(peer).value_or(float_format::QUAD)};
    const auto value{semantics::fold_float(*op, format, lhs, rhs)};
    return operand{gir::value::constant(pool, value, peer).constant_index()};
}

auto analyzer::analyze_shift(inst_id            inst,
                             const binary_info& info,
                             const operand&     lhs,
                             const operand&     rhs) -> result<operand> {
    const auto  kind{info.kind};
    const auto& src{src_of(inst)};
    auto&       shifted{type_of(lhs)};
    if (comptime_value(lhs) && comptime_value(rhs)) { return fold_shift(info, lhs, rhs, src); }

    // An untyped value shifted by a runtime amount takes the type it is used as
    if (shifted.get_kind() == gs::type_kind::COMPTIME_INT && comptime_value(lhs)) {
        return cast_ref{inst};
    }

    if (!gs::is_integer(shifted.get_kind())) {
        return comp_.unsupported(u_, src, "a shift of an untyped value");
    }

    TRY(require_runtime(src));
    const auto lv{TRY(as_runtime(lhs, src))};
    gir::value amount{};
    if (const auto index{comptime_value(rhs)}) {
        // An untyped amount takes the shifted type, clamped to its width
        const auto exact{int_of(comp_.constants().value_at(*index))};
        const auto domain{domain_of(shifted)};
        const auto small{exact ? exact->to_i128() : stdx::none};
        if (!small) { return comp_.unsupported(u_, src, "a wide shift amount"); }
        if (!info.wrapping && !info.saturating) {
            if (const auto probe{
                    semantics::fold_int_shift(semantics::int_op::SHL, domain, 0, *small)};
                !probe) {
                return comp_.report(u_,
                                    src,
                                    gs::error::COMPTIME_EVALUATION_FAILED,
                                    semantics::describe(probe.error(),
                                                        domain,
                                                        comp_.ctx().type_display_name(shifted)));
            }
        }

        // An amount the shifted type holds stays as written
        const u16  value_bits{static_cast<u16>(domain.is_signed ? domain.bits - 1 : domain.bits)};
        const bool in_range{*small >= 0 &&
                            (value_bits >= 127 || *small <= (i128{1} << value_bits) - 1)};
        const big_int clamped{in_range ? *small : i128{domain.bits}};
        amount = constant(intern_int(comp_.constants(), clamped, shifted), shifted);
    } else if (const auto widened{
                   TRY(widened_arithmetic(mod().data_of(inst).bin.rhs, shifted, src))}) {
        amount = *widened;
    } else {
        amount = TRY(as_runtime(rhs, src));
    }

    const auto op{semantics::int_op_of(kind, info.wrapping, info.saturating)};
    const bool checked{op && checks_overflow(*op, shifted)};
    return gir::value{builder_.emit_binary(kind, lv, amount, shifted, checked, info.saturating),
                      shifted};
}

auto analyzer::fold_shift(const binary_info& info,
                          const operand&     lhs,
                          const operand&     rhs,
                          const source&      src) -> result<operand> {
    auto&      pool{comp_.constants()};
    auto&      shifted{type_of(lhs)};
    const auto value{int_of(pool.value_at(*comptime_value(lhs)))};
    const auto amount{int_of(pool.value_at(*comptime_value(rhs)))};
    if (!value || !amount) { return comp_.unsupported(u_, src, "a shift of a non-integer"); }
    // An amount past i128 shifts out every bit just like one past the width
    constexpr auto I128_MAX{static_cast<i128>(~u128{0} >> 1)};
    const auto     small_amount{
        amount->to_i128().value_or(amount->is_negative() ? -I128_MAX - 1 : I128_MAX)};
    const auto fault{[&](semantics::int_fault f, semantics::int_domain domain) {
        return comp_.report(u_,
                            src,
                            gs::error::COMPTIME_EVALUATION_FAILED,
                            semantics::describe(f, domain, comp_.ctx().type_display_name(shifted)));
    }};

    // An untyped value shifts exactly, growing as far as the comptime_int limit
    if (shifted.get_kind() == gs::type_kind::COMPTIME_INT) {
        if (small_amount < 0) { return fault(semantics::int_fault::NEGATIVE_SHIFT, {}); }
        if (small_amount >= static_cast<i128>(MAX_COMPTIME_INT_BITS)) {
            return comp_.report(u_,
                                src,
                                gs::error::COMPTIME_EVALUATION_FAILED,
                                fmt::format("Compile-time integer result exceeds the {}-bit limit "
                                            "of comptime_int",
                                            MAX_COMPTIME_INT_BITS));
        }

        const auto bits{static_cast<u64>(small_amount)};
        return operand{
            intern_int(pool,
                       info.kind == instruction_kind::SHL ? value->shl(bits) : value->shr(bits),
                       shifted)};
    }

    if (!gs::is_integer(shifted.get_kind())) {
        return comp_.unsupported(u_, src, "a shift of this constant");
    }

    const auto op{*semantics::int_op_of(info.kind, info.wrapping, info.saturating)};
    const auto domain{domain_of(shifted)};
    if (!folds_exactly(shifted)) {
        const auto folded{semantics::fold_wide_shift(op, domain, *value, small_amount)};
        if (!folded) { return fault(folded.error(), domain); }
        return operand{intern_int(pool, *folded, shifted)};
    }

    const auto bits{semantics::to_domain(
        value->is_negative() ? static_cast<u128>(*value->to_i128()) : *value->to_u128(), domain)};
    const auto folded{semantics::fold_int_shift(op, domain, bits, small_amount)};
    if (!folded) { return fault(folded.error(), domain); }
    const auto out{domain.is_signed ? big_int{semantics::signed_value(*folded, domain)}
                                    : big_int{semantics::unsigned_value(*folded, domain)}};
    return operand{intern_int(pool, out, shifted)};
}

auto analyzer::fold_unary(tag t, const operand& value, gs::type& type) -> stdx::option<operand> {
    auto&      pool{comp_.constants()};
    const auto index{comptime_value(value)};
    if (!index) { return stdx::none; }
    const auto& v{pool.value_at(*index)};
    if (t == tag::bool_not) {
        const auto b{v.data.as_opt<bool>()};
        return b ? stdx::option<operand>{operand{pool.intern(!*b, type)}} : stdx::none;
    }

    if (t == tag::pos) { return value; }
    const bool negate{t != tag::bit_not};
    if (const auto f{v.data.as_opt<f128>()}; f && negate) {
        return operand{pool.intern(-*f, type)};
    }

    const auto exact{int_of(v)};
    if (!exact) { return stdx::none; }
    if (type.get_kind() == gs::type_kind::COMPTIME_INT) {
        // An untyped `~x` is its two's-complement complement at any width (`-x - 1`)
        const big_int one{i128{1}};
        return operand{intern_int(pool, negate ? -*exact : -*exact - one, type)};
    }

    // `-128i8`'s literal is past `i8`, so it negates exactly before it must fit
    if (negate && !gs::comptime_int_fits(*exact, type, comp_.ptr_bits())) {
        return operand{intern_int(pool, -*exact, type)};
    }

    const auto domain{domain_of(type)};
    const auto op{!negate              ? semantics::int_unary_op::NOT
                  : t == tag::neg_wrap ? semantics::int_unary_op::WRAP_NEG
                                       : semantics::int_unary_op::NEG};
    if (!folds_exactly(type)) {
        const auto wide{semantics::fold_wide_unary(op, domain, *exact)};
        if (!wide) { return stdx::none; }
        return operand{intern_int(pool, *wide, type)};
    }

    const auto bits{semantics::to_domain(
        exact->is_negative() ? static_cast<u128>(*exact->to_i128()) : *exact->to_u128(), domain)};
    const auto folded{semantics::fold_int_unary(op, domain, bits)};
    if (!folded) { return stdx::none; }
    const auto out{domain.is_signed ? big_int{semantics::signed_value(*folded, domain)}
                                    : big_int{semantics::unsigned_value(*folded, domain)}};
    return operand{intern_int(pool, out, type)};
}

auto analyzer::analyze_unary(inst_id inst, tag t) -> result<operand> {
    const auto& d{mod().data_of(inst)};
    const auto& src{src_of(inst)};
    const auto  operand_value{TRY(resolve_value(d.un.operand, src))};
    auto&       type{type_of(operand_value)};
    if (type.is_poison()) { return stdx::err{stop::POISON}; }
    auto& pool{comp_.constants()};
    if (vector_read(type)) { return vector_unary(t, operand_value, src); }

    // A lane read from a known vector folds, as the old emitter's evaluator sees through it
    if (operand_value.is<gir::value>()) {
        if (const auto built{built_value(operand_value)}) {
            if (const auto folded{fold_unary(t, operand{*built}, type)}) { return *folded; }
        }
    }

    switch (t) {
    case tag::pos:
        if (!gs::is_numeric(type.get_kind())) { break; }
        return operand_value;
    case tag::bool_not: {
        // `!p` of a pointer tests it against null
        if (type.get_kind() == gs::type_kind::POINTER && !comptime_value(operand_value)) {
            TRY(require_runtime(src));
            auto&      bool_type{builtin(gs::type_kind::BOOL)};
            const auto v{TRY(as_runtime(operand_value, src))};
            const auto null{constant(pool.intern(ip::nullptr_value{}, type), type)};
            return gir::value{builder_.emit_binary(instruction_kind::EQ, v, null, bool_type),
                              bool_type};
        }

        if (!is_bool(type)) { break; }
        if (const auto folded{fold_unary(t, operand_value, type)}) { return *folded; }

        TRY(require_runtime(src));
        const auto v{TRY(as_runtime(operand_value, src))};
        return gir::value{builder_.emit_unary(instruction_kind::NOT, v, type), type};
    }
    case tag::neg:
    case tag::neg_wrap:
    case tag::bit_not:  {
        const bool negate{t != tag::bit_not};
        if (!(negate ? gs::is_numeric(type.get_kind()) : is_int_like(type))) { break; }
        if (const auto index{comptime_value(operand_value)}) {
            const auto& v{pool.value_at(*index)};
            if (!v.data.is<f128>() && !int_of(v)) { break; }
            if (const auto folded{fold_unary(t, operand_value, type)}) { return *folded; }
        }

        TRY(require_runtime(src));
        const auto v{TRY(as_runtime(operand_value, src))};
        auto&      result_type{type_of(operand{v})};
        if (!negate) {
            return gir::value{builder_.emit_unary(instruction_kind::BITNOT, v, result_type),
                              result_type};
        }

        if (gs::is_integer(result_type.get_kind()) && !gs::is_signed_integer(result_type)) {
            if (t != tag::neg_wrap) {
                return comp_.unsupported(u_, src, "negating an unsigned integer");
            }

            // `NEG` is signed-only, so an unsigned `-%x` is the wrapping `0 -% x` it means
            const auto zero{constant(intern_int(pool, big_int{i128{0}}, result_type), result_type)};
            return gir::value{builder_.emit_binary(instruction_kind::SUB, zero, v, result_type),
                              result_type};
        }

        const bool checked{t == tag::neg && runtime_safety() &&
                           gs::is_integer(result_type.get_kind()) &&
                           semantics::any(semantics::runtime_checks(
                               semantics::int_unary_op::NEG,
                               {.bits = 1, .is_signed = gs::is_signed_integer(result_type)}))};
        return gir::value{builder_.emit_unary(instruction_kind::NEG, v, result_type, checked),
                          result_type};
    }
    default: break;
    }

    return comp_.unsupported(u_, src, "this unary operator on this type");
}

auto analyzer::bool_slot(inst_id inst, bool is_and) -> gir::value {
    auto& bool_type{builtin(gs::type_kind::BOOL)};
    auto& pool{comp_.constants()};

    // Made before the left side was analyzed, when it lowered to any instruction
    if (const auto made{bool_slots_.find(inst)}; made != bool_slots_.end()) {
        const auto slot{made->second};
        bool_slots_.erase(made);
        return slot;
    }

    const auto slot{builder_.emit_alloca(bool_type)};
    builder_.emit_store(slot, constant(pool.intern(!is_and, bool_type), bool_type));
    return gir::value{slot, bool_type};
}

auto analyzer::start_bool_ops(inst_id inst) -> void {
    // The default is stored before the left side runs, the outermost `and` / `or` first
    for (const auto op : comp_.bool_ops_starting_at(u_, inst)) {
        if (!bool_slots_.contains(op)) {
            bool_slots_.insert_or_assign(op, bool_slot(op, mod().tag_of(op) == tag::bool_and));
        }
    }
}

auto analyzer::analyze_bool_op(inst_id inst, bool is_and) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  lhs_ref{*rec.ref_field()};
    const auto  rhs_body{rec.body_field()};
    const auto  lhs{TRY(resolve_value(lhs_ref, src))};
    auto&       bool_type{builtin(gs::type_kind::BOOL)};

    // Both sides of a vector `and` / `or` run, combining lane by lane
    if (const auto vec{vector_read(type_of(lhs))};
        vec && is_bool(gs::vector_of(*vec)->underlying)) {
        bool_slots_.erase(inst);
        const auto out{TRY(analyze_body(rhs_body))};
        if (!out.yielded) { return comp_.unsupported(u_, src, "a right side that leaves"); }
        const binary_info info{.kind = is_and ? instruction_kind::AND : instruction_kind::OR,
                               .cls  = op_class::BITWISE};
        return vector_binary(info, lhs, *out.yielded, src);
    }

    if (!is_bool(type_of(lhs))) { return comp_.unsupported(u_, src, "`and` / `or` on a non-bool"); }
    auto& pool{comp_.constants()};

    // A known left side decides alone, or leaves the right side as the result
    if (const auto index{comptime_value(lhs)}) {
        const bool known{*pool.value_at(*index).data.as_opt<bool>()};
        if (known != is_and) { return lhs; }
        const auto out{TRY(analyze_body(rhs_body))};
        if (!out.yielded) { return comp_.unsupported(u_, src, "a right side that leaves"); }
        const auto rhs{
            TRY(coerce_to(*out.yielded, bool_type, src, {.what = use_site::kind::STORE}))};
        if (is_comptime() || !gir_fn_ || comptime_value(rhs)) { return rhs; }

        // A runtime body still keeps the result in its slot, as the old emitter does
        const auto slot{bool_slot(inst, is_and)};
        builder_.emit_store(slot, TRY(as_runtime(rhs, src)));
        return gir::value{builder_.emit_load(slot, bool_type), bool_type};
    }

    TRY(require_runtime(src));
    const auto cond{TRY(as_runtime(lhs, src))};
    const auto slot{bool_slot(inst, is_and)};
    const auto rhs_seg{new_segment()};
    const auto merge{new_segment()};
    if (is_and) {
        builder_.emit_cond_goto(cond, rhs_seg, merge);
    } else {
        builder_.emit_cond_goto(cond, merge, rhs_seg);
    }

    switch_to(rhs_seg);
    const auto out{[&] {
        const auto branch{runtime_branch()};
        return analyze_body(rhs_body);
    }()};
    if (!out) { return stdx::err{out.error()}; }
    if (!out->yielded || terminated()) {
        return comp_.unsupported(u_, src, "a right side that leaves");
    }

    const auto rhs{TRY(coerce_to(*out->yielded, bool_type, src, {.what = use_site::kind::STORE}))};
    builder_.emit_store(slot, TRY(as_runtime(rhs, src)));
    emit_goto(merge);
    switch_to(merge);
    return gir::value{builder_.emit_load(slot, bool_type), bool_type};
}

} // namespace ghoti::uir::sema
