#include "compiler/gir/semantics.hh"

#include <bit>
#include <limits>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/syntax/token_type.hh"
#include "support/float128.hh"
#include "support/float_math.hh"
#include "support/int128.hh"

namespace ghoti::gir::semantics {

namespace {

[[nodiscard]] auto mask_of(u16 bits) noexcept -> u128 {
    return bits >= 128 ? ~u128{0} : (u128{1} << bits) - 1;
}

[[nodiscard]] auto signed_min(u16 bits) noexcept -> i128 {
    return bits >= 128 ? std::numeric_limits<i128>::min() : -(i128{1} << (bits - 1));
}

[[nodiscard]] auto signed_max(u16 bits) noexcept -> i128 {
    return bits >= 128 ? std::numeric_limits<i128>::max() : (i128{1} << (bits - 1)) - 1;
}

[[nodiscard]] auto signed_fits(i128 value, u16 bits) noexcept -> bool {
    return value >= signed_min(bits) && value <= signed_max(bits);
}

[[nodiscard]] auto unsigned_fits(u128 value, u16 bits) noexcept -> bool {
    return value <= mask_of(bits);
}

[[nodiscard]] auto bits_of(i128 value, int_domain domain) noexcept -> u128 {
    return static_cast<u128>(value) & mask_of(domain.bits);
}

// Exact signed `+ - *`, or none past the 128-bit range (which is past every domain)
[[nodiscard]] auto exact_signed(int_op op, i128 l, i128 r) noexcept -> stdx::option<i128> {
    constexpr auto max{std::numeric_limits<i128>::max()};
    constexpr auto min{std::numeric_limits<i128>::min()};
    switch (op) {
    case int_op::ADD:
        if ((r > 0 && l > max - r) || (r < 0 && l < min - r)) { return stdx::none; }
        return l + r;
    case int_op::SUB:
        if ((r < 0 && l > max + r) || (r > 0 && l < min + r)) { return stdx::none; }
        return l - r;
    default: {
        if (l == 0 || r == 0) { return i128{0}; }
        if ((l == -1 && r == min) || (r == -1 && l == min)) { return stdx::none; }
        const auto product{l * r};
        if (product / r != l) { return stdx::none; }
        return product;
    }
    }
}

// Exact unsigned `+ - *`; none past 128 bits or below zero
[[nodiscard]] auto exact_unsigned(int_op op, u128 l, u128 r) noexcept -> stdx::option<u128> {
    switch (op) {
    case int_op::ADD: {
        const auto sum{l + r};
        if (sum < l) { return stdx::none; }
        return sum;
    }
    case int_op::SUB:
        if (l < r) { return stdx::none; }
        return l - r;
    default:
        if (l != 0 && r > ~u128{0} / l) { return stdx::none; }
        return l * r;
    }
}

[[nodiscard]] auto plain_op(int_op op) noexcept -> int_op {
    switch (op) {
    case int_op::WRAP_ADD:
    case int_op::SAT_ADD:  return int_op::ADD;
    case int_op::WRAP_SUB:
    case int_op::SAT_SUB:  return int_op::SUB;
    case int_op::WRAP_MUL:
    case int_op::SAT_MUL:  return int_op::MUL;
    default:               return op;
    }
}

// The low bits of `l op r`, which is also the wrapped result
[[nodiscard]] auto wrapped(int_op op, int_domain domain, u128 l, u128 r) noexcept -> u128 {
    switch (plain_op(op)) {
    case int_op::ADD: return to_domain(l + r, domain);
    case int_op::SUB: return to_domain(l - r, domain);
    default:          return to_domain(l * r, domain);
    }
}

// `l op r` clamped to the domain
[[nodiscard]] auto saturated(int_op op, int_domain domain, u128 l, u128 r) noexcept -> u128 {
    const auto base{plain_op(op)};
    if (domain.is_signed) {
        const auto sl{signed_value(l, domain)};
        const auto sr{signed_value(r, domain)};
        const auto exact{exact_signed(base, sl, sr)};
        bool       negative{};
        if (exact) {
            if (signed_fits(*exact, domain.bits)) { return bits_of(*exact, domain); }
            negative = *exact < 0;
        } else {
            // Past 128 bits the sign follows from the operands
            switch (base) {
            case int_op::ADD: negative = sr < 0; break;
            case int_op::SUB: negative = sr > 0; break;
            default:          negative = (sl < 0) != (sr < 0); break;
            }
        }
        return bits_of(negative ? signed_min(domain.bits) : signed_max(domain.bits), domain);
    }
    const auto exact{exact_unsigned(base, l, r)};
    if (exact && unsigned_fits(*exact, domain.bits)) { return *exact; }
    return base == int_op::SUB ? u128{0} : mask_of(domain.bits);
}

// The amount a plain shift may use, or the fault it raises
[[nodiscard]] auto checked_amount(int_domain domain, i128 amount) noexcept -> int_result<u16> {
    if (amount < 0) { return stdx::err{int_fault::NEGATIVE_SHIFT}; }
    if (amount >= domain.bits) { return stdx::err{int_fault::SHIFT_TOO_WIDE}; }
    return static_cast<u16>(amount);
}

[[nodiscard]] auto saturated_shl(int_domain domain, u128 l, i128 amount) noexcept -> u128 {
    if (l == 0) { return 0; }
    const auto in_range{checked_amount(domain, amount)};
    if (domain.is_signed) {
        const auto value{signed_value(l, domain)};
        const auto limit{
            bits_of(value < 0 ? signed_min(domain.bits) : signed_max(domain.bits), domain)};
        if (!in_range) { return limit; }
        const auto fits{value < 0 ? value >= (signed_min(domain.bits) >> *in_range)
                                  : value <= (signed_max(domain.bits) >> *in_range)};
        return fits ? to_domain(l << *in_range, domain) : limit;
    }
    if (!in_range || l > (mask_of(domain.bits) >> *in_range)) { return mask_of(domain.bits); }
    return to_domain(l << *in_range, domain);
}

// Truncating and flooring division; the caller has ruled out zero and `MIN / -1`
[[nodiscard]] auto divide(int_op op, int_domain domain, u128 l, u128 r) noexcept -> u128 {
    if (!domain.is_signed) {
        const auto ul{unsigned_value(l, domain)};
        const auto ur{unsigned_value(r, domain)};
        const bool remainder{op == int_op::REM || op == int_op::MOD};
        return remainder ? ul % ur : ul / ur;
    }
    const auto sl{signed_value(l, domain)};
    const auto sr{signed_value(r, domain)};
    auto       quotient{sl / sr};
    auto       remainder{sl % sr};
    const bool adjust{remainder != 0 && ((remainder < 0) != (sr < 0))};
    switch (op) {
    case int_op::DIV:       return bits_of(quotient, domain);
    case int_op::REM:       return bits_of(remainder, domain);
    case int_op::DIV_FLOOR: return bits_of(adjust ? quotient - 1 : quotient, domain);
    default:                return bits_of(adjust ? remainder + sr : remainder, domain);
    }
}

[[nodiscard]] auto count_leading_zeros(u128 value) noexcept -> u16 {
    const auto high{static_cast<u64>(value >> 64)};
    if (high != 0) { return static_cast<u16>(std::countl_zero(high)); }
    return static_cast<u16>(64 + std::countl_zero(static_cast<u64>(value)));
}

[[nodiscard]] auto count_trailing_zeros(u128 value) noexcept -> u16 {
    const auto low{static_cast<u64>(value)};
    if (low != 0) { return static_cast<u16>(std::countr_zero(low)); }
    return static_cast<u16>(64 + std::countr_zero(static_cast<u64>(value >> 64)));
}

} // namespace

auto int_op_of(syntax::token_type_t token) noexcept -> stdx::option<int_op> {
    using tt = syntax::token_type_t;
    switch (token) {
    case tt::PLUS:              return int_op::ADD;
    case tt::MINUS:             return int_op::SUB;
    case tt::STAR:              return int_op::MUL;
    case tt::SLASH:
    case tt::BUILTIN_DIV_TRUNC: return int_op::DIV;
    case tt::PERCENT:
    case tt::BUILTIN_REM:       return int_op::REM;
    case tt::PLUS_PERCENT:      return int_op::WRAP_ADD;
    case tt::MINUS_PERCENT:     return int_op::WRAP_SUB;
    case tt::STAR_PERCENT:      return int_op::WRAP_MUL;
    case tt::SHL_PERCENT:       return int_op::WRAP_SHL;
    case tt::PLUS_PIPE:         return int_op::SAT_ADD;
    case tt::MINUS_PIPE:        return int_op::SAT_SUB;
    case tt::STAR_PIPE:         return int_op::SAT_MUL;
    case tt::SHL_PIPE:          return int_op::SAT_SHL;
    case tt::BW_AND:            return int_op::AND;
    case tt::BW_OR:             return int_op::OR;
    case tt::CARET:             return int_op::XOR;
    case tt::SHL:               return int_op::SHL;
    case tt::SHR:               return int_op::SHR;
    case tt::BUILTIN_MIN:       return int_op::MIN;
    case tt::BUILTIN_MAX:       return int_op::MAX;
    case tt::BUILTIN_DIV_FLOOR: return int_op::DIV_FLOOR;
    case tt::BUILTIN_MOD:       return int_op::MOD;
    default:                    return stdx::none;
    }
}

auto int_op_of(instruction_kind kind, bool wrapping, bool saturating) noexcept
    -> stdx::option<int_op> {
    switch (kind) {
    case instruction_kind::ADD:
        return wrapping ? int_op::WRAP_ADD : saturating ? int_op::SAT_ADD : int_op::ADD;
    case instruction_kind::SUB:
        return wrapping ? int_op::WRAP_SUB : saturating ? int_op::SAT_SUB : int_op::SUB;
    case instruction_kind::MUL:
        return wrapping ? int_op::WRAP_MUL : saturating ? int_op::SAT_MUL : int_op::MUL;
    case instruction_kind::SHL:
        return wrapping ? int_op::WRAP_SHL : saturating ? int_op::SAT_SHL : int_op::SHL;
    case instruction_kind::DIV: return int_op::DIV;
    case instruction_kind::MOD: return int_op::REM;
    case instruction_kind::SHR: return int_op::SHR;
    case instruction_kind::AND: return int_op::AND;
    case instruction_kind::OR:  return int_op::OR;
    case instruction_kind::XOR: return int_op::XOR;
    default:                    return stdx::none;
    }
}

auto int_compare_op_of(syntax::token_type_t token) noexcept -> stdx::option<int_compare_op> {
    using tt = syntax::token_type_t;
    switch (token) {
    case tt::EQ:    return int_compare_op::EQ;
    case tt::NEQ:   return int_compare_op::NE;
    case tt::LT:    return int_compare_op::LT;
    case tt::LT_EQ: return int_compare_op::LE;
    case tt::GT:    return int_compare_op::GT;
    case tt::GT_EQ: return int_compare_op::GE;
    default:        return stdx::none;
    }
}

auto describe(int_fault fault, int_domain domain, std::string_view type_name) -> std::string {
    switch (fault) {
    case int_fault::OVERFLOW_:
        return fmt::format(
            "Signed integer overflow in compile-time constant expression: the result "
            "does not fit '{}'",
            type_name);
    case int_fault::DIVISION_BY_ZERO: return "Division by zero in compile-time constant expression";
    case int_fault::MODULO_BY_ZERO:   return "Modulo by zero in compile-time constant expression";
    case int_fault::NEGATIVE_SHIFT:
        return "Negative shift amount in compile-time constant expression";
    case int_fault::SHIFT_TOO_WIDE:
        return fmt::format("Shift amount is not less than the {}-bit width of the shifted value",
                           domain.bits);
    case int_fault::OUT_OF_RANGE:
        return fmt::format("Integer value is out of range for type '{}'", type_name);
    }
    return {};
}

auto signed_value(u128 bits, int_domain domain) noexcept -> i128 {
    const auto masked{to_domain(bits, domain)};
    if (domain.bits >= 128) { return static_cast<i128>(masked); }
    const bool negative{((masked >> (domain.bits - 1)) & 1) != 0};
    return negative ? static_cast<i128>(masked) - (i128{1} << domain.bits)
                    : static_cast<i128>(masked);
}

auto unsigned_value(u128 bits, int_domain domain) noexcept -> u128 {
    return to_domain(bits, domain);
}

auto to_domain(u128 bits, int_domain domain) noexcept -> u128 {
    return bits & mask_of(domain.bits);
}

auto fold_int(int_op op, int_domain domain, u128 lhs, u128 rhs) -> int_result<u128> {
    const auto l{to_domain(lhs, domain)};
    const auto r{to_domain(rhs, domain)};
    switch (op) {
    case int_op::ADD:
    case int_op::SUB:
    case int_op::MUL:
        // Unsigned arithmetic wraps; signed arithmetic must stay in range
        if (!domain.is_signed) { return wrapped(op, domain, l, r); }
        if (const auto exact{exact_signed(op, signed_value(l, domain), signed_value(r, domain))};
            exact && signed_fits(*exact, domain.bits)) {
            return bits_of(*exact, domain);
        }
        return stdx::err{int_fault::OVERFLOW_};
    case int_op::WRAP_ADD:
    case int_op::WRAP_SUB:
    case int_op::WRAP_MUL: return wrapped(op, domain, l, r);
    case int_op::SAT_ADD:
    case int_op::SAT_SUB:
    case int_op::SAT_MUL:  return saturated(op, domain, l, r);
    case int_op::SHL:
    case int_op::SHR:
    case int_op::WRAP_SHL:
    case int_op::SAT_SHL:  {
        const auto amount{domain.is_signed ? signed_value(r, domain)
                                           : static_cast<i128>(unsigned_value(r, domain))};
        // A 128-bit unsigned amount past the signed range is still past every width
        const bool huge{!domain.is_signed && domain.bits >= 128 && (r >> 127) != 0};
        return fold_int_shift(op, domain, l, huge ? i128{128} : amount);
    }
    case int_op::AND: return l & r;
    case int_op::OR:  return l | r;
    case int_op::XOR: return l ^ r;
    case int_op::MIN:
    case int_op::MAX: {
        const bool less{domain.is_signed ? signed_value(l, domain) < signed_value(r, domain)
                                         : l < r};
        return (op == int_op::MIN) == less ? l : r;
    }
    case int_op::DIV:
    case int_op::REM:
    case int_op::DIV_FLOOR:
    case int_op::MOD:
        if (r == 0) {
            const bool remainder{op == int_op::REM || op == int_op::MOD};
            return stdx::err{remainder ? int_fault::MODULO_BY_ZERO : int_fault::DIVISION_BY_ZERO};
        }
        if (domain.is_signed && signed_value(l, domain) == signed_min(domain.bits) &&
            signed_value(r, domain) == -1) {
            return stdx::err{int_fault::OVERFLOW_};
        }
        return divide(op, domain, l, r);
    }
    return stdx::err{int_fault::OVERFLOW_};
}

auto is_shift(int_op op) noexcept -> bool {
    return op == int_op::SHL || op == int_op::SHR || op == int_op::WRAP_SHL ||
           op == int_op::SAT_SHL;
}

auto fold_int_shift(int_op op, int_domain domain, u128 lhs, i128 amount) -> int_result<u128> {
    const auto l{to_domain(lhs, domain)};
    switch (op) {
    case int_op::WRAP_SHL:
        // Shifting every bit out leaves nothing, whatever the amount
        if (amount < 0 || amount >= domain.bits) { return u128{0}; }
        return to_domain(l << static_cast<u16>(amount), domain);
    case int_op::SAT_SHL: return saturated_shl(domain, l, amount);
    case int_op::SHL:     {
        const auto checked{checked_amount(domain, amount)};
        if (!checked) { return stdx::err{checked.error()}; }
        return to_domain(l << *checked, domain);
    }
    default: {
        const auto checked{checked_amount(domain, amount)};
        if (!checked) { return stdx::err{checked.error()}; }
        if (domain.is_signed) { return bits_of(signed_value(l, domain) >> *checked, domain); }
        return l >> *checked;
    }
    }
}

auto fold_int_unary(int_unary_op op, int_domain domain, u128 value) -> int_result<u128> {
    const auto v{to_domain(value, domain)};
    switch (op) {
    case int_unary_op::NEG:
        if (domain.is_signed && signed_value(v, domain) == signed_min(domain.bits)) {
            return stdx::err{int_fault::OVERFLOW_};
        }
        return to_domain(u128{0} - v, domain);
    case int_unary_op::WRAP_NEG: return to_domain(u128{0} - v, domain);
    case int_unary_op::NOT:      return to_domain(~v, domain);
    case int_unary_op::ABS:
        if (!domain.is_signed || signed_value(v, domain) >= 0) { return v; }
        if (signed_value(v, domain) == signed_min(domain.bits)) {
            return stdx::err{int_fault::OVERFLOW_};
        }
        return to_domain(u128{0} - v, domain);
    case int_unary_op::CLZ:
        return v == 0 ? u128{domain.bits}
                      : u128{static_cast<u64>(count_leading_zeros(v) - (128 - domain.bits))};
    case int_unary_op::CTZ:
        return v == 0 ? u128{domain.bits} : u128{static_cast<u64>(count_trailing_zeros(v))};
    case int_unary_op::POPCOUNT:
        return u128{static_cast<u64>(std::popcount(static_cast<u64>(v)) +
                                     std::popcount(static_cast<u64>(v >> 64)))};
    }
    return v;
}

auto compare_int(int_compare_op op, int_domain domain, u128 lhs, u128 rhs) noexcept -> bool {
    const auto l{to_domain(lhs, domain)};
    const auto r{to_domain(rhs, domain)};
    const bool less{domain.is_signed ? signed_value(l, domain) < signed_value(r, domain) : l < r};
    const bool equal{l == r};
    switch (op) {
    case int_compare_op::EQ: return equal;
    case int_compare_op::NE: return !equal;
    case int_compare_op::LT: return less;
    case int_compare_op::LE: return less || equal;
    case int_compare_op::GT: return !less && !equal;
    case int_compare_op::GE: return !less;
    }
    return false;
}

auto cast_int(int_domain from, int_domain to, u128 value) -> int_result<u128> {
    if (from.is_signed) {
        const auto exact{signed_value(value, from)};
        const bool fits{to.is_signed
                            ? signed_fits(exact, to.bits)
                            : exact >= 0 && unsigned_fits(static_cast<u128>(exact), to.bits)};
        if (!fits) { return stdx::err{int_fault::OUT_OF_RANGE}; }
        return bits_of(exact, to);
    }
    const auto exact{unsigned_value(value, from)};
    const bool fits{to.is_signed ? exact <= static_cast<u128>(signed_max(to.bits))
                                 : unsigned_fits(exact, to.bits)};
    if (!fits) { return stdx::err{int_fault::OUT_OF_RANGE}; }
    return exact;
}

auto truncate_int(int_domain to, u128 value) noexcept -> u128 { return to_domain(value, to); }

auto runtime_checks(int_op op, int_domain domain) noexcept -> int_checks {
    switch (op) {
    case int_op::ADD:
    case int_op::SUB:
    case int_op::MUL:       return {.overflow = domain.is_signed};
    case int_op::DIV:
    case int_op::REM:
    case int_op::DIV_FLOOR:
    case int_op::MOD:       return {.division = true};
    case int_op::SHL:
    case int_op::SHR:       return {.shift_amount = true};
    default:                return {};
    }
}

auto runtime_checks(int_unary_op op, int_domain domain) noexcept -> int_checks {
    switch (op) {
    case int_unary_op::NEG:
    case int_unary_op::ABS: return {.overflow = domain.is_signed};
    default:                return {};
    }
}

auto float_op_of(syntax::token_type_t token) noexcept -> stdx::option<float_op> {
    using tt = syntax::token_type_t;
    switch (token) {
    case tt::PLUS:        return float_op::ADD;
    case tt::MINUS:       return float_op::SUB;
    case tt::STAR:        return float_op::MUL;
    case tt::SLASH:       return float_op::DIV;
    case tt::PERCENT:     return float_op::REM;
    case tt::BUILTIN_MIN: return float_op::MIN;
    case tt::BUILTIN_MAX: return float_op::MAX;
    default:              return stdx::none;
    }
}

auto math_function_of(syntax::token_type_t token) noexcept -> stdx::option<math_function> {
    using enum syntax::token_type_t;
    switch (token) {
    case BUILTIN_SQRT:  return math_function::SQRT;
    case BUILTIN_SIN:   return math_function::SIN;
    case BUILTIN_COS:   return math_function::COS;
    case BUILTIN_TAN:   return math_function::TAN;
    case BUILTIN_EXP:   return math_function::EXP;
    case BUILTIN_EXP2:  return math_function::EXP2;
    case BUILTIN_LOG:   return math_function::LOG;
    case BUILTIN_LOG2:  return math_function::LOG2;
    case BUILTIN_LOG10: return math_function::LOG10;
    case BUILTIN_FLOOR: return math_function::FLOOR;
    case BUILTIN_CEIL:  return math_function::CEIL;
    default:            return stdx::none;
    }
}

auto float_from_int(int_domain from, u128 value, float_format format) -> f128 {
    if (from.is_signed) { return f128::from_int(signed_value(value, from), format); }
    return f128::from_uint(unsigned_value(value, from), format);
}

auto int_from_float(f128 value, int_domain to) -> int_result<u128> {
    if (!value.is_finite()) { return stdx::err{int_fault::OUT_OF_RANGE}; }
    const auto whole{value.trunc()};
    if (whole.is_zero()) { return u128{0}; }
    if (to.is_signed) {
        const auto exact{whole.to_int()};
        if (!exact || !signed_fits(*exact, to.bits)) { return stdx::err{int_fault::OUT_OF_RANGE}; }
        return bits_of(*exact, to);
    }
    const auto exact{whole.is_negative() ? stdx::none : whole.to_uint()};
    if (!exact || !unsigned_fits(*exact, to.bits)) { return stdx::err{int_fault::OUT_OF_RANGE}; }
    return *exact;
}

auto fold_float(float_op op, float_format format, f128 lhs, f128 rhs) -> f128 {
    switch (op) {
    case float_op::ADD: return add(lhs, rhs, format);
    case float_op::SUB: return subtract(lhs, rhs, format);
    case float_op::MUL: return multiply(lhs, rhs, format);
    case float_op::DIV: return divide(lhs, rhs, format);
    case float_op::REM: return remainder_trunc(lhs, rhs, format);
    case float_op::MIN:
    case float_op::MAX: {
        if (lhs.is_nan()) { return rhs; }
        if (rhs.is_nan()) { return lhs; }
        const bool want_max{op == float_op::MAX};
        // Equal zeros differ only in sign, which decides between them
        if (lhs.is_zero() && rhs.is_zero()) { return (lhs.is_negative() != want_max) ? lhs : rhs; }
        return (want_max ? lhs > rhs : lhs < rhs) ? lhs : rhs;
    }
    }
    return lhs;
}

} // namespace ghoti::gir::semantics
