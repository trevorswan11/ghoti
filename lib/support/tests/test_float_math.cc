#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <stdx/types.hh>

#include "support/float128.hh"
#include "support/float_math.hh"
#include "support/int128.hh"

namespace ghoti::tests {

namespace {

using F = math_function;
using M = float_format;

struct oracle_vector {
    math_function function;
    float_format  format;
    u64           input_high;
    u64           input_low;
    u64           expected_high;
    u64           expected_low;
};

// mpmath results rounded into each format; see tools/gen_float_math_vectors.py
const std::vector<oracle_vector> ORACLE{
#include "data/float_math_vectors.inc"
};

constexpr std::array ALL_FORMATS{M::HALF, M::SINGLE, M::DOUBLE, M::X87, M::QUAD};
constexpr std::array APPROXIMATED{
    F::SIN, F::COS, F::TAN, F::EXP, F::EXP2, F::LOG, F::LOG2, F::LOG10};
constexpr std::array ALL_FUNCTIONS{
    F::SQRT, F::SIN, F::COS, F::TAN, F::EXP, F::EXP2, F::LOG, F::LOG2, F::LOG10, F::FLOOR, F::CEIL};

[[nodiscard]] auto quad(u64 high, u64 low) -> f128 {
    return f128::from_bits((u128{high} << 64) | u128{low});
}

[[nodiscard]] auto parsed(std::string_view text) -> f128 { return f128::parse(text).value; }

[[nodiscard]] auto same_bits(f128 lhs, f128 rhs) -> bool { return lhs.bits() == rhs.bits(); }

} // namespace

TEST_CASE("compile-time math matches the mpmath oracle in every format") {
    REQUIRE(ORACLE.size() > 4'000);
    usize                    mismatches{0};
    std::vector<std::string> first_mismatches;
    u32                      most_working_bits{0};
    for (const auto& vector : ORACLE) {
        const auto input{quad(vector.input_high, vector.input_low)};
        const auto expected{quad(vector.expected_high, vector.expected_low)};
        const auto result{evaluate_traced(vector.function, input, vector.format)};
        most_working_bits = std::max(most_working_bits, result.working_bits);
        if (same_bits(result.value, expected)) { continue; }
        ++mismatches;
        if (first_mismatches.size() < 10) {
            first_mismatches.emplace_back(fmt::format("{}({}) into format {}: got {}, want {}",
                                                      math_function_name(vector.function),
                                                      input,
                                                      static_cast<u32>(vector.format),
                                                      result.value,
                                                      expected));
        }
    }
    INFO(fmt::format("{}", fmt::join(first_mismatches, "\n")));
    CHECK(mismatches == 0);
    // Even the hardest roundings and the largest arguments settle far below the limit
    CHECK(most_working_bits <= MAX_MATH_WORKING_BITS / 8);
}

TEST_CASE("compile-time math follows IEEE 754 for special values") {
    const auto nan{f128::quiet_nan()};
    const auto inf{f128::infinity()};
    const auto neg_inf{f128::infinity(true)};
    const auto zero{f128::zero()};
    const auto neg_zero{f128::zero(true)};
    const auto one{f128::from_int(1)};

    for (const auto format : ALL_FORMATS) {
        for (const auto function : ALL_FUNCTIONS) {
            CHECK(evaluate(function, nan, format).is_nan());
        }

        CHECK(same_bits(evaluate(F::SQRT, neg_zero, format), neg_zero));
        CHECK(same_bits(evaluate(F::SQRT, zero, format), zero));
        CHECK(evaluate(F::SQRT, -one, format).is_nan());
        CHECK(evaluate(F::SQRT, neg_inf, format).is_nan());
        CHECK(same_bits(evaluate(F::SQRT, inf, format), inf));

        for (const auto function : {F::LOG, F::LOG2, F::LOG10}) {
            CHECK(same_bits(evaluate(function, zero, format), neg_inf));
            CHECK(same_bits(evaluate(function, neg_zero, format), neg_inf));
            CHECK(evaluate(function, -one, format).is_nan());
            CHECK(evaluate(function, neg_inf, format).is_nan());
            CHECK(same_bits(evaluate(function, inf, format), inf));
            CHECK(same_bits(evaluate(function, one, format), zero));
        }

        for (const auto function : {F::EXP, F::EXP2}) {
            CHECK(same_bits(evaluate(function, neg_inf, format), zero));
            CHECK(same_bits(evaluate(function, inf, format), inf));
            CHECK(same_bits(evaluate(function, zero, format), one));
            CHECK(same_bits(evaluate(function, neg_zero, format), one));
            CHECK_FALSE(evaluate_traced(function, inf, format).overflowed);
        }

        for (const auto function : {F::SIN, F::COS, F::TAN}) {
            CHECK(evaluate(function, inf, format).is_nan());
            CHECK(evaluate(function, neg_inf, format).is_nan());
        }
        for (const auto function : {F::SIN, F::TAN}) {
            CHECK(same_bits(evaluate(function, zero, format), zero));
            CHECK(same_bits(evaluate(function, neg_zero, format), neg_zero));
        }
        CHECK(same_bits(evaluate(F::COS, zero, format), one));
        CHECK(same_bits(evaluate(F::COS, neg_zero, format), one));

        for (const auto function : {F::FLOOR, F::CEIL}) {
            CHECK(same_bits(evaluate(function, inf, format), inf));
            CHECK(same_bits(evaluate(function, neg_inf, format), neg_inf));
            CHECK(same_bits(evaluate(function, zero, format), zero));
            CHECK(same_bits(evaluate(function, neg_zero, format), neg_zero));
        }
    }
}

TEST_CASE("compile-time math returns exact results without approximating") {
    for (const auto format : ALL_FORMATS) {
        for (i64 k{-14}; k <= 15; ++k) {
            const auto power{evaluate(F::EXP2, f128::from_int(k), format)};
            const auto back{evaluate_traced(F::LOG2, power, format)};
            CHECK(same_bits(back.value, f128::from_int(k)));
            CHECK(back.working_bits == 0);
        }
        auto power_of_ten{f128::from_int(1)};
        for (i64 k{0}; k <= 4; ++k) {
            const auto log{evaluate_traced(F::LOG10, power_of_ten, format)};
            CHECK(same_bits(log.value, f128::from_int(k)));
            CHECK(log.working_bits == 0);
            power_of_ten = power_of_ten * f128::from_int(10);
        }
    }
    // The largest power of ten a quad holds exactly
    CHECK(same_bits(evaluate(F::LOG10, parsed("1e48")), f128::from_int(48)));
    CHECK(evaluate_traced(F::LOG10, parsed("1e49"), M::QUAD).working_bits != 0);

    CHECK(same_bits(evaluate(F::SQRT, f128::from_int(144)), f128::from_int(12)));
    CHECK(same_bits(evaluate(F::SQRT, parsed("0.25")), parsed("0.5")));
    CHECK(same_bits(evaluate(F::FLOOR, parsed("-0.5")), f128::from_int(-1)));
    CHECK(same_bits(evaluate(F::FLOOR, parsed("0.5")), f128::zero()));
    CHECK(same_bits(evaluate(F::CEIL, parsed("-0.5")), f128::zero(true)));
    CHECK(same_bits(evaluate(F::CEIL, parsed("0.5")), f128::from_int(1)));
    CHECK(same_bits(evaluate(F::FLOOR, parsed("-3.0")), f128::from_int(-3)));
    CHECK(same_bits(evaluate(F::CEIL, parsed("1e40")), parsed("1e40")));
}

TEST_CASE("compile-time exp reports a finite result past the format's range") {
    const auto thousand{f128::from_int(1'000)};
    CHECK(evaluate_traced(F::EXP, thousand, M::DOUBLE).overflowed);
    CHECK(evaluate_traced(F::EXP, thousand, M::DOUBLE).value.is_infinite());
    CHECK_FALSE(evaluate_traced(F::EXP, thousand, M::QUAD).overflowed);
    CHECK(evaluate_traced(F::EXP, parsed("1e30"), M::QUAD).overflowed);
    CHECK(evaluate_traced(F::EXP2, f128::from_int(1'024), M::DOUBLE).overflowed);
    CHECK(evaluate_traced(F::EXP2, parsed("1023.5"), M::DOUBLE).value.is_finite());
    CHECK(evaluate_traced(F::EXP2, parsed("1e30"), M::QUAD).overflowed);

    // Underflow is a rounding, not an error
    const auto tiny{evaluate_traced(F::EXP, -thousand, M::DOUBLE)};
    CHECK(same_bits(tiny.value, f128::zero()));
    CHECK_FALSE(tiny.overflowed);
    CHECK(same_bits(evaluate(F::EXP, parsed("-1e30")), f128::zero()));
    CHECK(same_bits(evaluate(F::EXP2, f128::from_int(-1'074), M::DOUBLE), parsed("0x1p-1074")));
    CHECK(same_bits(evaluate(F::EXP2, f128::from_int(-1'076), M::DOUBLE), f128::zero()));
}

TEST_CASE("compile-time math settles arguments at the edge of the exponent range") {
    const auto smallest{parsed("0x1p-16494")};
    const auto largest{parsed("0x1.ffffffffffffffffffffffffffffp16383")};
    // sin x and tan x round to x itself that close to zero
    CHECK(same_bits(evaluate(F::SIN, smallest), smallest));
    CHECK(same_bits(evaluate(F::TAN, -smallest), -smallest));
    CHECK(same_bits(evaluate(F::COS, smallest), f128::from_int(1)));
    CHECK(same_bits(evaluate(F::EXP, smallest), f128::from_int(1)));
    CHECK(same_bits(evaluate(F::EXP2, -smallest), f128::from_int(1)));

    for (const auto function : {F::SIN, F::COS, F::TAN}) {
        const auto result{evaluate_traced(function, largest, M::QUAD)};
        CHECK(result.value.is_finite());
        CHECK(result.working_bits <= MAX_MATH_WORKING_BITS / 8);
    }
    // sin^2 + cos^2 of a huge argument is one to within rounding
    const auto sine{evaluate(F::SIN, largest)};
    const auto cosine{evaluate(F::COS, largest)};
    const auto unit{(sine * sine) + (cosine * cosine)};
    CHECK((unit - f128::from_int(1)).abs() < parsed("0x1p-110"));
}

TEST_CASE("compile-time sqrt, floor, and ceil agree with the host on doubles") {
    std::mt19937_64 engine{0x5eed};
    for (usize i{0}; i < 20'000; ++i) {
        const auto value{std::bit_cast<f64>(engine())};
        if (!std::isfinite(value)) { continue; }
        const auto x{f128::from_f64(value)};
        CHECK(same_bits(evaluate(F::FLOOR, x, M::DOUBLE), f128::from_f64(std::floor(value))));
        CHECK(same_bits(evaluate(F::CEIL, x, M::DOUBLE), f128::from_f64(std::ceil(value))));
        if (value >= 0) {
            CHECK(same_bits(evaluate(F::SQRT, x, M::DOUBLE), f128::from_f64(std::sqrt(value))));
        }
    }
}

TEST_CASE("a narrower format's result is the wider result's correct rounding or its neighbor") {
    // Rounding the quad result again is right except at double-rounding cases, where the two
    // differ by one unit in the last place: never more
    std::mt19937_64 engine{0xfeed};
    for (usize i{0}; i < 300; ++i) {
        const auto x{f128::from_f64(std::bit_cast<f64>((engine() >> 12) | 0x3ff0000000000000))};
        for (const auto function : APPROXIMATED) {
            const auto direct{evaluate(function, x, M::DOUBLE).to_f64()};
            const auto twice{evaluate(function, x, M::QUAD).to_f64()};
            CHECK((direct == twice || std::nextafter(direct, twice) == twice));
        }
    }
}

TEST_CASE("folding ten thousand quad sines stays fast") {
    std::mt19937_64 engine{0xc0ffee};
    const auto      start{std::chrono::steady_clock::now()};
    for (usize i{0}; i < 10'000; ++i) {
        // Any finite quad: the exponent field stops short of all ones
        const u64  high{engine() & 0xfffeffffffffffff};
        const auto x{quad(high, engine())};
        CHECK(evaluate(F::SIN, x).abs() <= f128::from_int(1));
    }
    const auto elapsed{std::chrono::steady_clock::now() - start};
    CHECK(elapsed < std::chrono::seconds{60});
}

} // namespace ghoti::tests
