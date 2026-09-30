#include "support/float_math.hh"

#include <bit>
#include <string_view>
#include <utility>

#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "support/big_uint.hh"
#include "support/float128.hh"
#include "support/int128.hh"

// This guy's almost all claude... good luck!

namespace ghoti {

namespace {

[[nodiscard]] auto big(u64 value) -> big_uint { return big_uint{u128{value}}; }

[[nodiscard]] auto one(u64 precision) -> big_uint {
    auto value{big(1)};
    value.shift_left(precision);
    return value;
}

[[nodiscard]] auto sum(big_uint lhs, const big_uint& rhs) -> big_uint {
    lhs.add(rhs);
    return lhs;
}

// Requires `lhs >= rhs`
[[nodiscard]] auto difference(big_uint lhs, const big_uint& rhs) -> big_uint {
    lhs.subtract(rhs);
    return lhs;
}

// The fixed-point product, truncated
[[nodiscard]] auto multiply(const big_uint& lhs, const big_uint& rhs, u64 precision) -> big_uint {
    auto product{lhs * rhs};
    product.shift_right(precision);
    return product;
}

[[nodiscard]] auto quotient(big_uint numerator, const big_uint& divisor) -> big_uint {
    return big_uint::divide(numerator, divisor);
}

[[nodiscard]] auto significant_bits(u128 value) noexcept -> i64 {
    return value.high != 0 ? 64 + std::bit_width(value.high) : std::bit_width(value.low);
}

// Exponent of the value's leading bit: `2^top <= |x| < 2^(top + 1)`
[[nodiscard]] auto top_exponent(const float_parts& x) noexcept -> i64 {
    return x.exponent + significant_bits(x.significand) - 1;
}

// `|x|` at `precision`, truncated when it has bits below the last place
[[nodiscard]] auto to_fixed(const float_parts& x, u64 precision) -> big_uint {
    big_uint   value{x.significand};
    const auto shift{x.exponent + static_cast<i64>(precision)};
    if (shift >= 0) {
        value.shift_left(static_cast<u64>(shift));
    } else {
        value.shift_right(static_cast<u64>(-shift));
    }
    return value;
}

struct signed_fixed {
    big_uint magnitude;
    bool     negative{false};
};

[[nodiscard]] auto add(signed_fixed lhs, const signed_fixed& rhs) -> signed_fixed {
    if (lhs.negative == rhs.negative) {
        lhs.magnitude.add(rhs.magnitude);
        return lhs;
    }
    if (lhs.magnitude >= rhs.magnitude) {
        lhs.magnitude.subtract(rhs.magnitude);
        return lhs;
    }
    return {difference(rhs.magnitude, lhs.magnitude), rhs.negative};
}

// `center * 2^exponent`, off from the true value by at most `error` units of the center
struct approximation {
    signed_fixed center;
    big_uint     error;
    i64          exponent;
};

// The sum over k of `1 / ((2k + 1) * n^(2k + 1))`, which is `atanh(1 / n)`, or `atan(1 / n)` when
// the terms alternate. Off by less than one unit.
[[nodiscard]] auto inverse_series(u32 n, u64 precision, bool alternating) -> big_uint {
    constexpr u64 GUARD_BITS{32};
    auto          power{one(precision + GUARD_BITS)};
    power.divide_small(n);
    big_uint added{power};
    big_uint removed;
    for (u32 k{1}; !power.is_zero(); ++k) {
        power.divide_small(n * n);
        auto term{power};
        term.divide_small((2 * k) + 1);
        (alternating && k % 2 == 1 ? removed : added).add(term);
    }
    added.subtract(removed);
    added.shift_right(GUARD_BITS);
    return added;
}

// `ln 2 = 2 * atanh(1 / 3)`
[[nodiscard]] auto compute_ln2(u64 precision) -> big_uint {
    auto value{inverse_series(3, precision + 4, false)};
    value.shift_right(3);
    return value;
}

// `ln 10 = 3 * ln 2 + ln 1.25`, with `ln 1.25 = 2 * atanh(1 / 9)`
[[nodiscard]] auto compute_ln10(u64 precision) -> big_uint {
    auto       ln2_half{inverse_series(3, precision + 4, false)};
    const auto ln_five_quarters_half{inverse_series(9, precision + 4, false)};
    ln2_half.multiply_add_small(3, 0);
    ln2_half.add(ln_five_quarters_half);
    ln2_half.shift_right(3);
    return ln2_half;
}

// Machin: `pi = 16 * atan(1 / 5) - 4 * atan(1 / 239)`
[[nodiscard]] auto compute_pi(u64 precision) -> big_uint {
    auto large{inverse_series(5, precision + 8, true)};
    auto small{inverse_series(239, precision + 8, true)};
    large.shift_left(4);
    small.shift_left(2);
    large.subtract(small);
    large.shift_right(8);
    return large;
}

// A constant computed once at the highest precision asked for and cut down for smaller requests.
// What `at` returns is off by less than two units.
class cached_constant {
  public:
    using generator = big_uint (*)(u64 precision);

  public:
    explicit cached_constant(generator compute) noexcept : compute_{compute} {}

    [[nodiscard]] auto at(u64 precision) -> big_uint {
        if (precision > precision_) {
            constexpr u64 STEP{1'024};
            precision_ = ((precision + STEP - 1) / STEP) * STEP;
            value_     = compute_(precision_);
        }
        auto value{value_};
        value.shift_right(precision_ - precision);
        return value;
    }

  private:
    generator compute_;
    u64       precision_{0};
    big_uint  value_;
};

[[nodiscard]] auto ln2_at(u64 precision) -> big_uint {
    thread_local cached_constant cache{compute_ln2};
    return cache.at(precision);
}

[[nodiscard]] auto ln10_at(u64 precision) -> big_uint {
    thread_local cached_constant cache{compute_ln10};
    return cache.at(precision);
}

[[nodiscard]] auto pi_at(u64 precision) -> big_uint {
    thread_local cached_constant cache{compute_pi};
    return cache.at(precision);
}

[[nodiscard]] auto compute_two_over_pi(u64 precision) -> big_uint {
    const auto pi_precision{precision + 16};
    return quotient(one(precision + 1 + pi_precision), pi_at(pi_precision));
}

[[nodiscard]] auto two_over_pi_at(u64 precision) -> big_uint {
    thread_local cached_constant cache{compute_two_over_pi};
    return cache.at(precision);
}

struct bounded {
    big_uint value;
    big_uint error;
};

// `numerator / denominator` with the error both bounds leave in the quotient; none while the
// denominator can't be told from zero
[[nodiscard]] auto divide_bounded(const bounded& numerator,
                                  const bounded& denominator,
                                  u64            precision) -> stdx::option<bounded> {
    if (denominator.value <= sum(denominator.error, denominator.error)) { return stdx::none; }
    const auto least_denominator{difference(denominator.value, denominator.error)};

    auto scaled{numerator.value};
    scaled.shift_left(precision);
    auto result{quotient(std::move(scaled), denominator.value)};

    // |n/d - n*/d*| <= (|n - n*| + |n*/d*| * |d - d*|) / d
    auto greatest{sum(numerator.value, numerator.error)};
    greatest.shift_left(precision);
    const auto greatest_quotient{sum(quotient(std::move(greatest), least_denominator), big(1))};
    auto       slack{numerator.error};
    slack.shift_left(precision);
    slack.add(greatest_quotient * denominator.error);
    auto error{sum(quotient(std::move(slack), least_denominator), big(3))};
    return bounded{std::move(result), std::move(error)};
}

struct reduced_angle {
    big_uint remainder; // in [0, pi/2), off by at most 8 units
    u32      quadrant;
};

// `|x| = quadrant * pi/2 + remainder`, exact for any magnitude: `2/pi` is taken to as many bits
// as the argument's exponent needs
[[nodiscard]] auto reduce_angle(const float_parts& x, u64 precision) -> reduced_angle {
    const auto top{top_exponent(x)};
    if (top < 0) { return {to_fixed(x, precision), 0}; }

    const auto constant_precision{static_cast<u64>(top) + 1 + precision + 8};
    auto       turns{big_uint{x.significand} * two_over_pi_at(constant_precision)};
    turns.shift_right(static_cast<u64>(static_cast<i64>(constant_precision) - x.exponent) -
                      precision);
    const u32 quadrant{(turns.bit(precision) ? 1U : 0U) | (turns.bit(precision + 1) ? 2U : 0U)};
    turns.keep_low(precision);
    return {multiply(turns, pi_at(precision), precision + 1), quadrant};
}

struct sine_cosine {
    big_uint sine;
    big_uint cosine;
    big_uint error;
};

// Taylor series for an angle in [0, pi/2)
[[nodiscard]] auto sine_cosine_of(const big_uint& angle, u64 precision) -> sine_cosine {
    const auto square{multiply(angle, angle, precision)};
    big_uint   sine_added{angle};
    big_uint   sine_removed;
    big_uint   cosine_added{one(precision)};
    big_uint   cosine_removed;
    auto       sine_term{angle};
    auto       cosine_term{one(precision)};
    u32        terms{0};
    for (u32 k{1}; !sine_term.is_zero() || !cosine_term.is_zero(); ++k, ++terms) {
        cosine_term = multiply(cosine_term, square, precision);
        cosine_term.divide_small(((2 * k) - 1) * (2 * k));
        sine_term = multiply(sine_term, square, precision);
        sine_term.divide_small((2 * k) * ((2 * k) + 1));
        const bool removes{k % 2 == 1};
        (removes ? cosine_removed : cosine_added).add(cosine_term);
        (removes ? sine_removed : sine_added).add(sine_term);
    }
    const auto settle{[](big_uint added, const big_uint& removed) {
        return added >= removed ? difference(std::move(added), removed) : big_uint{};
    }};
    return {settle(std::move(sine_added), sine_removed),
            settle(std::move(cosine_added), cosine_removed),
            big(64 + (8ULL * terms))};
}

[[nodiscard]] auto approximate_trig(math_function function, const float_parts& x, u64 precision)
    -> stdx::option<approximation> {
    const auto reduced{reduce_angle(x, precision)};
    auto       parts{sine_cosine_of(reduced.remainder, precision)};
    const auto quadrant{reduced.quadrant};
    const bool swapped{quadrant % 2 == 1};
    const auto exponent{-static_cast<i64>(precision)};

    if (function == math_function::SIN) {
        return approximation{
            {std::move(swapped ? parts.cosine : parts.sine), (quadrant >= 2) != x.negative},
            std::move(parts.error),
            exponent};
    }
    if (function == math_function::COS) {
        return approximation{
            {std::move(swapped ? parts.sine : parts.cosine), quadrant == 1 || quadrant == 2},
            std::move(parts.error),
            exponent};
    }
    const bounded sine{std::move(parts.sine), parts.error};
    const bounded cosine{std::move(parts.cosine), parts.error};
    auto          tangent{swapped ? divide_bounded(cosine, sine, precision)
                                  : divide_bounded(sine, cosine, precision)};
    if (!tangent) { return stdx::none; }
    return approximation{
        {std::move(tangent->value), swapped != x.negative}, std::move(tangent->error), exponent};
}

struct series_sum {
    big_uint value;
    u32      terms;
};

// Taylor series of `e^r` for `r` in [0, 1)
[[nodiscard]] auto exp_series(const big_uint& r, u64 precision) -> series_sum {
    auto total{one(precision)};
    auto term{one(precision)};
    u32  terms{0};
    for (u32 n{1};; ++n, ++terms) {
        term = multiply(term, r, precision);
        term.divide_small(n);
        if (term.is_zero()) { break; }
        total.add(term);
    }
    return {std::move(total), terms};
}

// `e^x = 2^k * e^r` with `r` in [0, ln 2]; `|x| < 2^15` keeps `k` small
[[nodiscard]] auto approximate_exp(const float_parts& x, u64 precision) -> approximation {
    const auto ln2{ln2_at(precision)};
    auto       remainder{to_fixed(x, precision)};
    auto       k{big_uint::divide(remainder, ln2).low_u128().low};
    if (x.negative) {
        remainder = difference(ln2, remainder);
        ++k;
    }
    auto [value, terms]{exp_series(remainder, precision)};
    return {{std::move(value), false},
            big(64 + (8ULL * terms) + (8 * (k + 1))),
            (x.negative ? -static_cast<i64>(k) : static_cast<i64>(k)) -
                static_cast<i64>(precision)};
}

// `2^x = 2^i * e^(f * ln 2)` with `f` in [0, 1)
[[nodiscard]] auto approximate_exp2(const float_parts& x, u64 precision) -> approximation {
    auto fraction{to_fixed(x, precision)};
    auto whole{fraction};
    whole.shift_right(precision);
    auto integer{static_cast<i64>(whole.low_u128().low)};
    fraction.keep_low(precision);
    if (x.negative) {
        integer = -integer;
        if (!fraction.is_zero()) {
            fraction = difference(one(precision), fraction);
            --integer;
        }
    }
    const auto power{multiply(fraction, ln2_at(precision), precision)};
    auto [value, terms]{exp_series(power, precision)};
    return {
        {std::move(value), false}, big(64 + (8ULL * terms)), integer - static_cast<i64>(precision)};
}

// `x = mantissa * 2^exponent` with the mantissa in [0.75, 1.5)
struct natural_log {
    signed_fixed mantissa_log;
    big_uint     mantissa_error;
    i64          exponent;
};

// `ln m = 2 * atanh((m - 1) / (m + 1))`
[[nodiscard]] auto log_of_mantissa(const float_parts& x, u64 precision) -> natural_log {
    const auto width{significant_bits(x.significand)};
    big_uint   numerator{x.significand};
    auto       unit{one(static_cast<u64>(width - 1))};
    auto       exponent{x.exponent + width - 1};
    // At 1.5 and above, halve the mantissa so the series argument stays small
    if (sum(numerator, numerator) >= sum(sum(unit, unit), unit)) {
        unit.shift_left(1);
        ++exponent;
    }
    const bool below_one{numerator < unit};
    auto       gap{below_one ? difference(unit, numerator) : difference(numerator, unit)};
    gap.shift_left(precision);
    const auto argument{quotient(std::move(gap), sum(numerator, unit))};

    const auto square{multiply(argument, argument, precision)};
    auto       power{argument};
    auto       total{argument};
    u32        terms{0};
    for (u32 n{1};; ++n, ++terms) {
        power = multiply(power, square, precision);
        if (power.is_zero()) { break; }
        auto term{power};
        term.divide_small((2 * n) + 1);
        total.add(term);
    }
    total.shift_left(1);
    return {{std::move(total), below_one}, big(16 + (8ULL * terms)), exponent};
}

[[nodiscard]] auto magnitude_of(i64 value) noexcept -> u64 {
    return value < 0 ? static_cast<u64>(-value) : static_cast<u64>(value);
}

struct bounded_signed {
    signed_fixed value;
    big_uint     error;
};

// `ln x = exponent * ln 2 + ln mantissa`
[[nodiscard]] auto natural_log_of(const float_parts& x, u64 precision) -> bounded_signed {
    auto       log{log_of_mantissa(x, precision)};
    const auto scale{magnitude_of(log.exponent)};
    auto       exponent_part{ln2_at(precision)};
    exponent_part.multiply_add_small(static_cast<u32>(scale), 0);
    auto value{add(std::move(log.mantissa_log), {std::move(exponent_part), log.exponent < 0})};
    return {std::move(value), sum(std::move(log.mantissa_error), big((2 * scale) + 2))};
}

[[nodiscard]] auto approximate_log(math_function function, const float_parts& x, u64 precision)
    -> stdx::option<approximation> {
    const auto exponent{-static_cast<i64>(precision)};
    if (function == math_function::LOG) {
        auto log{natural_log_of(x, precision)};
        return approximation{std::move(log.value), std::move(log.error), exponent};
    }
    if (function == math_function::LOG10) {
        auto       log{natural_log_of(x, precision)};
        const auto scaled{divide_bounded({std::move(log.value.magnitude), std::move(log.error)},
                                         {ln10_at(precision), big(2)},
                                         precision)};
        if (!scaled) { return stdx::none; }
        return approximation{{scaled->value, log.value.negative}, scaled->error, exponent};
    }
    // `log2 x = exponent + ln mantissa / ln 2`
    auto       log{log_of_mantissa(x, precision)};
    const auto scaled{divide_bounded({std::move(log.mantissa_log.magnitude), log.mantissa_error},
                                     {ln2_at(precision), big(2)},
                                     precision)};
    if (!scaled) { return stdx::none; }
    auto whole{big(magnitude_of(log.exponent))};
    whole.shift_left(precision);
    return approximation{
        add({std::move(whole), log.exponent < 0}, {scaled->value, log.mantissa_log.negative}),
        scaled->error,
        exponent};
}

[[nodiscard]] auto approximate(math_function function, const float_parts& x, u64 precision)
    -> stdx::option<approximation> {
    switch (function) {
    case math_function::SIN:
    case math_function::COS:
    case math_function::TAN:   return approximate_trig(function, x, precision);
    case math_function::EXP:   return approximate_exp(x, precision);
    case math_function::EXP2:  return approximate_exp2(x, precision);
    case math_function::LOG:
    case math_function::LOG2:
    case math_function::LOG10: return approximate_log(function, x, precision);
    default:                   return stdx::none;
    }
}

// The rounded value when the whole error interval agrees on it
[[nodiscard]] auto decide(const approximation& approx, float_format format) -> stdx::option<f128> {
    if (approx.center.magnitude <= approx.error) { return stdx::none; }
    const auto negative{approx.center.negative};
    const auto low{round_scaled(negative,
                                difference(approx.center.magnitude, approx.error),
                                approx.exponent,
                                false,
                                format)};
    const auto high{round_scaled(
        negative, sum(approx.center.magnitude, approx.error), approx.exponent, false, format)};
    if (low.bits() != high.bits()) { return stdx::none; }
    return low;
}

// A value just above (or below) `significand * 2^exponent`, by less than one unit of it
[[nodiscard]] auto
nudged(bool negative, big_uint significand, i64 exponent, bool away, float_format format) -> f128 {
    if (!away) { significand.subtract(big(1)); }
    return round_scaled(negative, std::move(significand), exponent, true, format);
}

[[nodiscard]] auto floor_of(f128 x) -> f128 {
    if (!x.is_finite() || x.is_zero()) { return x; }
    const auto truncated{x.trunc()};
    if (!x.is_negative() || truncated == x) { return truncated; }
    return subtract(truncated, f128::from_int(1));
}

[[nodiscard]] auto sqrt_of(f128 x, float_format format) -> f128 {
    if (x.is_zero()) { return x; }
    if (x.is_negative()) { return f128::quiet_nan(); }
    if (x.is_infinite()) { return x; }

    // Widened so the integer root carries more bits than any format keeps, at an even exponent
    const auto    parts{x.parts()};
    constexpr i64 RADICAND_BITS{240};
    auto          shift{RADICAND_BITS - significant_bits(parts.significand)};
    if ((parts.exponent - shift) % 2 != 0) { ++shift; }
    big_uint radicand{parts.significand};
    radicand.shift_left(static_cast<u64>(shift));

    big_uint root;
    for (u64 bit{(radicand.bit_length() / 2) + 1}; bit-- > 0;) {
        const auto candidate{sum(root, one(bit))};
        if (candidate * candidate <= radicand) { root = candidate; }
    }
    const bool inexact{root * root != radicand};
    return round_scaled(false, std::move(root), (parts.exponent - shift) / 2, inexact, format);
}

// Whether `x` is `10^k` for an integer `k >= 0`, the only inputs with an exact `log10`
[[nodiscard]] auto power_of_ten(const float_parts& x) -> stdx::option<i64> {
    if (x.negative || x.significand == 0) { return stdx::none; }
    auto odd{x.significand};
    auto twos{x.exponent};
    while ((odd & 1) == 0) {
        odd >>= 1;
        ++twos;
    }
    // 5^49 is the first power a quad significand can't hold
    if (twos < 0 || twos > 48) { return stdx::none; }
    u128 power_of_five{1};
    for (i64 k{0}; k < twos; ++k) { power_of_five *= 5; }
    if (odd != power_of_five) { return stdx::none; }
    return twos;
}

constexpr i64 UNIT_BITS{240};

// Results that need no approximation: special values, exact results, and arguments so small or
// so large that the rounding is already decided
[[nodiscard]] auto settled(math_function function, f128 x, float_format format)
    -> stdx::option<math_result> {
    const bool is_log{function == math_function::LOG || function == math_function::LOG2 ||
                      function == math_function::LOG10};
    const bool is_exp{function == math_function::EXP || function == math_function::EXP2};
    if (is_log) {
        if (x.is_zero()) { return math_result{f128::infinity(true)}; }
        if (x.is_negative()) { return math_result{f128::quiet_nan()}; }
        if (x.is_infinite()) { return math_result{x}; }
    } else if (is_exp) {
        if (x.is_infinite()) { return math_result{x.is_negative() ? f128::zero() : x}; }
        if (x.is_zero()) { return math_result{f128::from_int(1)}; }
    } else {
        if (x.is_infinite()) { return math_result{f128::quiet_nan()}; }
        if (x.is_zero()) {
            return math_result{function == math_function::COS ? f128::from_int(1) : x};
        }
    }

    const auto parts{x.parts()};
    const auto top{top_exponent(parts)};
    if (is_log) {
        const auto width{significant_bits(parts.significand)};
        const bool power_of_two{parts.significand == (u128{1} << static_cast<u32>(width - 1))};
        if (power_of_two && top == 0) { return math_result{f128::zero()}; }
        if (function == math_function::LOG2 && power_of_two) {
            return math_result{f128::from_int(i128{top}, format)};
        }
        if (function == math_function::LOG10) {
            if (const auto k{power_of_ten(parts)}) {
                return math_result{f128::from_int(i128{*k}, format)};
            }
        }
        return stdx::none;
    }

    if (is_exp) {
        // Past every format's range in either direction
        if (top >= 15) {
            if (parts.negative) {
                return math_result{nudged(false, big(1), -100'000, true, format)};
            }
            return math_result{.value = f128::infinity(), .overflowed = true};
        }
        if (function == math_function::EXP2 && x.trunc() == x) {
            const auto exponent{static_cast<i64>(*x.to_int())};
            const auto value{round_scaled(false, big(1), exponent, false, format)};
            return math_result{.value = value, .overflowed = value.is_infinite()};
        }
        // 1 + x + ... is within 2^-200 of one
        if (top < -(UNIT_BITS + 1)) {
            return math_result{nudged(false, one(UNIT_BITS), -UNIT_BITS, !parts.negative, format)};
        }
        return stdx::none;
    }

    // cos x = 1 - x^2/2 + ...
    if (function == math_function::COS) {
        if (top < -(UNIT_BITS / 2)) {
            return math_result{nudged(false, one(UNIT_BITS), -UNIT_BITS, false, format)};
        }
        return stdx::none;
    }
    // sin x = x - x^3/6 + ... and tan x = x + x^3/3 + ...: within a quarter unit of x itself
    if (top < -64) {
        big_uint significand{parts.significand};
        significand.shift_left(2);
        return math_result{nudged(parts.negative,
                                  std::move(significand),
                                  parts.exponent - 2,
                                  function == math_function::TAN,
                                  format)};
    }
    return stdx::none;
}

} // namespace

auto math_function_name(math_function function) noexcept -> std::string_view {
    switch (function) {
    case math_function::SQRT:  return "sqrt";
    case math_function::SIN:   return "sin";
    case math_function::COS:   return "cos";
    case math_function::TAN:   return "tan";
    case math_function::EXP:   return "exp";
    case math_function::EXP2:  return "exp2";
    case math_function::LOG:   return "log";
    case math_function::LOG2:  return "log2";
    case math_function::LOG10: return "log10";
    case math_function::FLOOR: return "floor";
    case math_function::CEIL:  return "ceil";
    }
    std::unreachable();
}

auto evaluate_traced(math_function function, f128 x, float_format format) -> math_result {
    if (x.is_nan()) { return {f128::quiet_nan()}; }
    switch (function) {
    case math_function::FLOOR: return {floor_of(x).round_to(format)};
    case math_function::CEIL:  return {(-floor_of(-x)).round_to(format)};
    case math_function::SQRT:  return {sqrt_of(x, format)};
    default:                   break;
    }
    if (const auto known{settled(function, x, format)}) { return *known; }

    const auto parts{x.parts()};
    const bool is_exp{function == math_function::EXP || function == math_function::EXP2};
    for (u32 precision{format_info(format).precision + 64}; precision <= MAX_MATH_WORKING_BITS;
         precision *= 2) {
        const auto approx{approximate(function, parts, precision)};
        if (!approx) { continue; }
        if (const auto value{decide(*approx, format)}) {
            return {.value        = *value,
                    .working_bits = precision,
                    .overflowed   = is_exp && value->is_infinite()};
        }
    }
    VERIFY(false, "compile-time math needed more working precision than its limit");
    return {f128::quiet_nan()};
}

auto evaluate(math_function function, f128 x, float_format format) -> f128 {
    return evaluate_traced(function, x, format).value;
}

} // namespace ghoti
