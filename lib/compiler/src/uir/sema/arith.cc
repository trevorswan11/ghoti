#include <array>
#include <compare>

#include <fmt/format.h>
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

// A comptime_int grows as needed, so only a runaway result such as `1 << (1 << 40)` is refused
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

[[nodiscard]] auto is_int_like(const gs::type& t) noexcept -> bool {
    return gs::is_integer(t.get_kind()) || t.get_kind() == gs::type_kind::COMPTIME_INT;
}

[[nodiscard]] auto is_bool(const gs::type& t) noexcept -> bool {
    return t.get_kind() == gs::type_kind::BOOL;
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
    case op_class::EQUALITY:   return numeric || (is_bool(lhs) && is_bool(rhs));
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
    return comp_.ctx().runtime_safety && gs::is_integer(t.get_kind()) &&
           semantics::any(
               semantics::runtime_checks(op, {.bits = 1, .is_signed = gs::is_signed_integer(t)}));
}

auto analyzer::analyze_binary(inst_id inst, tag t) -> result<operand> {
    const auto& d{mod().data_of(inst)};
    const auto& src{src_of(inst)};
    const auto  lhs{TRY(resolve_value(d.bin.lhs, src))};
    const auto  rhs{TRY(resolve_value(d.bin.rhs, src))};
    auto&       lhs_type{type_of(lhs)};
    auto&       rhs_type{type_of(rhs)};
    if (lhs_type.is_poison() || rhs_type.is_poison()) { return stdx::err{stop::POISON}; }

    const auto info{binary_info_of(t)};
    if (!applies(info.cls, lhs_type, rhs_type)) {
        return comp_.report(
            u_,
            src,
            gs::error::OPERATOR_TYPE_MISMATCH,
            messages::operator_mismatch(comp_.ctx(), operator_spelling(t), lhs_type, rhs_type));
    }
    if (info.cls == op_class::SHIFT) { return analyze_shift(inst, info, lhs, rhs); }

    const std::array operands{lhs, rhs};
    auto&            peer{*TRY(peer_type(operands, src))};
    // Untyped operands only stay untyped when both are known, and fold
    const bool     both_known{comptime_value(lhs) && comptime_value(rhs)};
    auto&          work{both_known || !gs::is_comptime_numeric(peer.get_kind())
                            ? peer
                            : comp_.ctx().default_concrete(peer)};
    const use_site as_operand{.what = use_site::kind::STORE};
    const auto     l{TRY(coerce_to(lhs, work, src, as_operand))};
    const auto     r{TRY(coerce_to(rhs, work, src, as_operand))};

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
        const bool equal{*lb == *rb};
        return operand{pool.intern(info.kind == instruction_kind::EQ ? equal : !equal, bool_type)};
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

    if (peer.get_kind() == gs::type_kind::COMPTIME_INT) {
        if (info.wrapping || info.saturating) {
            return comp_.unsupported(u_, src, "a wrapping or saturating operator on comptime_int");
        }
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
    if (!folds_exactly(peer)) {
        if (is_comptime()) {
            return comp_.unsupported(u_, src, "compile-time arithmetic past 128 bits");
        }
        return stdx::none;
    }
    const auto domain{domain_of(peer)};
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
    if (comptime_value(lhs) && comptime_value(rhs) && folds_exactly(shifted)) {
        return fold_shift(info, lhs, rhs, src);
    }
    if (!gs::is_integer(shifted.get_kind()) || comptime_value(lhs)) {
        return comp_.unsupported(u_, src, "a shift of a constant or untyped value");
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
        const bool    in_range{*small >= 0 && *small < domain.bits};
        const big_int clamped{in_range ? *small : i128{domain.bits}};
        amount = constant(intern_int(comp_.constants(), clamped, shifted), shifted);
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
    const auto small_amount{amount->to_i128()};
    const auto fault{[&](semantics::int_fault f, semantics::int_domain domain) {
        return comp_.report(u_,
                            src,
                            gs::error::COMPTIME_EVALUATION_FAILED,
                            semantics::describe(f, domain, comp_.ctx().type_display_name(shifted)));
    }};

    // An untyped value shifts exactly, growing as far as the comptime_int limit
    if (shifted.get_kind() == gs::type_kind::COMPTIME_INT) {
        if (info.wrapping || info.saturating) {
            return comp_.unsupported(u_, src, "a wrapping or saturating shift of comptime_int");
        }
        if (!small_amount || *small_amount < 0) {
            return fault(semantics::int_fault::NEGATIVE_SHIFT, {});
        }
        if (*small_amount >= static_cast<i128>(MAX_COMPTIME_INT_BITS)) {
            return comp_.report(u_,
                                src,
                                gs::error::COMPTIME_EVALUATION_FAILED,
                                fmt::format("Compile-time integer result exceeds the {}-bit limit "
                                            "of comptime_int",
                                            MAX_COMPTIME_INT_BITS));
        }
        const auto bits{static_cast<u64>(*small_amount)};
        return operand{
            intern_int(pool,
                       info.kind == instruction_kind::SHL ? value->shl(bits) : value->shr(bits),
                       shifted)};
    }

    if (!gs::is_integer(shifted.get_kind()) || !small_amount) {
        return comp_.unsupported(u_, src, "a shift of this constant");
    }
    const auto op{*semantics::int_op_of(info.kind, info.wrapping, info.saturating)};
    const auto domain{domain_of(shifted)};
    const auto bits{semantics::to_domain(
        value->is_negative() ? static_cast<u128>(*value->to_i128()) : *value->to_u128(), domain)};
    const auto folded{semantics::fold_int_shift(op, domain, bits, *small_amount)};
    if (!folded) { return fault(folded.error(), domain); }
    const auto out{domain.is_signed ? big_int{semantics::signed_value(*folded, domain)}
                                    : big_int{semantics::unsigned_value(*folded, domain)}};
    return operand{intern_int(pool, out, shifted)};
}

auto analyzer::analyze_unary(inst_id inst, tag t) -> result<operand> {
    const auto& d{mod().data_of(inst)};
    const auto& src{src_of(inst)};
    const auto  operand_value{TRY(resolve_value(d.un.operand, src))};
    auto&       type{type_of(operand_value)};
    if (type.is_poison()) { return stdx::err{stop::POISON}; }
    auto& pool{comp_.constants()};

    switch (t) {
    case tag::pos:
        if (!gs::is_numeric(type.get_kind())) { break; }
        return operand_value;
    case tag::bool_not: {
        if (!is_bool(type)) { break; }
        if (const auto index{comptime_value(operand_value)}) {
            return operand{pool.intern(!*pool.value_at(*index).data.as_opt<bool>(), type)};
        }
        TRY(require_runtime(src));
        const auto v{TRY(as_runtime(operand_value, src))};
        return gir::value{builder_.emit_unary(instruction_kind::NOT, v, type), type};
    }
    case tag::neg:
    case tag::neg_wrap:
    case tag::bit_not:  {
        const bool negate{t != tag::bit_not};
        if (!(negate ? gs::is_numeric(type.get_kind()) : is_int_like(type))) { break; }
        if (const auto index{comptime_value(operand_value)}; index && folds_exactly(type)) {
            const auto& v{pool.value_at(*index)};
            if (const auto f{v.data.as_opt<f128>()}; f && negate) {
                return operand{pool.intern(-*f, type)};
            }
            const auto value{int_of(v)};
            if (!value) { break; }
            if (type.get_kind() == gs::type_kind::COMPTIME_INT) {
                // An untyped `~x` is `-x - 1`, its two's-complement complement at any width
                const big_int one{i128{1}};
                return operand{intern_int(pool, negate ? -*value : -*value - one, type)};
            }
            // `-128i8`'s literal is past `i8`, so it negates exactly before it must fit
            if (negate && !gs::comptime_int_fits(*value, type, comp_.ptr_bits())) {
                return operand{intern_int(pool, -*value, type)};
            }
            const auto domain{domain_of(type)};
            const auto bits{semantics::to_domain(
                value->is_negative() ? static_cast<u128>(*value->to_i128()) : *value->to_u128(),
                domain)};
            const auto op{!negate              ? semantics::int_unary_op::NOT
                          : t == tag::neg_wrap ? semantics::int_unary_op::WRAP_NEG
                                               : semantics::int_unary_op::NEG};
            if (const auto folded{semantics::fold_int_unary(op, domain, bits)}) {
                const auto out{domain.is_signed
                                   ? big_int{semantics::signed_value(*folded, domain)}
                                   : big_int{semantics::unsigned_value(*folded, domain)}};
                return operand{intern_int(pool, out, type)};
            }
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
        const bool checked{t == tag::neg && comp_.ctx().runtime_safety &&
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

auto analyzer::analyze_bool_op(inst_id inst, bool is_and) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  lhs_ref{*rec.ref_field()};
    const auto  rhs_body{rec.body_field()};
    const auto  lhs{TRY(resolve_value(lhs_ref, src))};
    auto&       bool_type{builtin(gs::type_kind::BOOL)};
    if (!is_bool(type_of(lhs))) { return comp_.unsupported(u_, src, "`and` / `or` on a non-bool"); }
    auto& pool{comp_.constants()};

    // A known left side decides alone, or leaves the right side as the result
    if (const auto index{comptime_value(lhs)}) {
        const bool known{*pool.value_at(*index).data.as_opt<bool>()};
        if (known != is_and) { return lhs; }
        const auto out{TRY(analyze_body(rhs_body))};
        if (!out.yielded) { return comp_.unsupported(u_, src, "a right side that leaves"); }
        return coerce_to(*out.yielded, bool_type, src, {.what = use_site::kind::STORE});
    }

    TRY(require_runtime(src));
    const auto cond{TRY(as_runtime(lhs, src))};
    const auto slot{builder_.emit_alloca(bool_type)};
    builder_.emit_store(slot, constant(pool.intern(!is_and, bool_type), bool_type));
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
