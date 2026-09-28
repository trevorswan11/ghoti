#include <bit>
#include <charconv>
#include <cmath>
#include <compare>
#include <limits>
#include <random>
#include <string>
#include <string_view>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/types.hh>

#include "support/float128.hh"
#include "support/int128.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

// Random finite doubles spread over the whole exponent range, subnormals included
class double_source {
  public:
    auto next() -> f64 {
        while (true) {
            const auto value{std::bit_cast<f64>(bits_(engine_))};
            if (std::isfinite(value)) { return value; }
        }
    }

    auto next_float() -> f32 {
        while (true) {
            const auto value{std::bit_cast<f32>(static_cast<u32>(bits_(engine_)))};
            if (std::isfinite(value)) { return value; }
        }
    }

  private:
    std::mt19937_64                    engine_{0x5eed};
    std::uniform_int_distribution<u64> bits_;
};

[[nodiscard]] auto same_double(f64 expected, f64 actual) -> bool {
    if (std::isnan(expected)) { return std::isnan(actual); }
    return std::bit_cast<u64>(expected) == std::bit_cast<u64>(actual);
}

[[nodiscard]] auto same_float(f32 expected, f128 actual) -> bool {
    const auto bits{static_cast<u32>(actual.encode(float_format::SINGLE).low)};
    if (std::isnan(expected)) { return actual.is_nan(); }
    return std::bit_cast<u32>(expected) == bits;
}

[[nodiscard]] auto from_float(f32 value) -> f128 {
    return f128::decode(u128{std::bit_cast<u32>(value)}, float_format::SINGLE);
}

[[nodiscard]] auto parse_value(std::string_view text, float_format format = float_format::QUAD)
    -> f128 {
    const auto result{f128::parse(text, format)};
    REQUIRE(result.status != float_parse_status::MALFORMED);
    return result.value;
}

} // namespace

TEST_CASE("f128 round-trips every double exactly") {
    double_source source;
    for (i32 i{0}; i < 20'000; ++i) {
        const auto value{source.next()};
        INFO(value);
        CHECK(same_double(value, f128::from_f64(value).to_f64()));
    }
    for (const f64 special : {0.0,
                              -0.0,
                              std::numeric_limits<f64>::infinity(),
                              -std::numeric_limits<f64>::infinity(),
                              std::numeric_limits<f64>::denorm_min(),
                              std::numeric_limits<f64>::max()}) {
        CHECK(same_double(special, f128::from_f64(special).to_f64()));
    }
    CHECK(f128::from_f64(std::numeric_limits<f64>::quiet_NaN()).is_nan());
}

TEST_CASE("f128 arithmetic rounded to double matches hardware doubles") {
    double_source source;
    for (i32 i{0}; i < 20'000; ++i) {
        const auto a{source.next()};
        const auto b{source.next()};
        const auto c{source.next()};
        const auto x{f128::from_f64(a)};
        const auto y{f128::from_f64(b)};
        const auto z{f128::from_f64(c)};
        INFO(fmt::format("{:a} {:a} {:a}", a, b, c));
        CHECK(same_double(a + b, add(x, y, float_format::DOUBLE).to_f64()));
        CHECK(same_double(a - b, subtract(x, y, float_format::DOUBLE).to_f64()));
        CHECK(same_double(a * b, multiply(x, y, float_format::DOUBLE).to_f64()));
        CHECK(same_double(a / b, divide(x, y, float_format::DOUBLE).to_f64()));
        CHECK(same_double(std::fma(a, b, c),
                          fused_multiply_add(x, y, z, float_format::DOUBLE).to_f64()));
    }
}

TEST_CASE("f128 arithmetic near one exercises the rounding boundaries") {
    // Nearby operands keep results normal and make ties and cancellations common
    std::mt19937_64                    engine{7};
    std::uniform_int_distribution<i32> offset{-8, 8};
    for (i32 i{0}; i < 20'000; ++i) {
        const auto a{std::ldexp(1.0 + (offset(engine) * 0x1p-52), offset(engine))};
        const auto b{std::ldexp(1.0 + (offset(engine) * 0x1p-52), offset(engine) - 53)};
        const auto x{f128::from_f64(a)};
        const auto y{f128::from_f64(b)};
        INFO(fmt::format("{:a} {:a}", a, b));
        CHECK(same_double(a + b, add(x, y, float_format::DOUBLE).to_f64()));
        CHECK(same_double(a - b, subtract(x, y, float_format::DOUBLE).to_f64()));
        CHECK(same_double(a * b, multiply(x, y, float_format::DOUBLE).to_f64()));
        CHECK(same_double(a / b, divide(x, y, float_format::DOUBLE).to_f64()));
    }
}

TEST_CASE("f128 arithmetic rounded to single matches hardware floats") {
    double_source source;
    for (i32 i{0}; i < 20'000; ++i) {
        const auto a{source.next_float()};
        const auto b{source.next_float()};
        const auto x{from_float(a)};
        const auto y{from_float(b)};
        INFO(fmt::format("{:a} {:a}", a, b));
        CHECK(same_float(a + b, add(x, y, float_format::SINGLE)));
        CHECK(same_float(a * b, multiply(x, y, float_format::SINGLE)));
        CHECK(same_float(a / b, divide(x, y, float_format::SINGLE)));
        // A double holds every float exactly, so narrowing it is the only rounding
        CHECK(same_float(static_cast<f32>(static_cast<f64>(a) * 1.1),
                         f128::from_f64(static_cast<f64>(a) * 1.1).round_to(float_format::SINGLE)));
    }
}

TEST_CASE("f128 parses decimal text exactly like from_chars") {
    double_source source;
    for (i32 i{0}; i < 5'000; ++i) {
        const auto value{source.next()};
        // Many digits past the shortest form, so the text is rarely an exact double
        const auto text{fmt::format("{:.25e}", value)};
        f64        expected{};
        std::from_chars(text.data(), text.data() + text.size(), expected);
        INFO(text);
        CHECK(same_double(expected, parse_value(text, float_format::DOUBLE).to_f64()));
    }

    // Halfway cases between neighboring doubles must round to even
    CHECK(parse_value("9007199254740993", float_format::DOUBLE).to_f64() == 9007199254740992.0);
    CHECK(parse_value("9007199254740995", float_format::DOUBLE).to_f64() == 9007199254740996.0);
    CHECK(parse_value("0.1", float_format::DOUBLE).to_f64() == 0.1);
    CHECK(parse_value("2.2250738585072011e-308", float_format::DOUBLE).to_f64() ==
          2.2250738585072011e-308);
    CHECK(parse_value("4.9406564584124654e-324", float_format::DOUBLE).to_f64() ==
          std::numeric_limits<f64>::denorm_min());
}

TEST_CASE("f128 parses hex text exactly") {
    CHECK(parse_value("0x1.8p3").to_f64() == 12.0);
    CHECK(parse_value("0X1P-4").to_f64() == 0.0625);
    CHECK(parse_value("0xA.8").to_f64() == 10.5);
    CHECK(parse_value("-0x1p-1074", float_format::DOUBLE).to_f64() ==
          -std::numeric_limits<f64>::denorm_min());
    CHECK(parse_value("0x1.fffffffffffff8p1023", float_format::DOUBLE).is_infinite());
    CHECK(parse_value("0x1.ffffffffffffffffffffffffffffp16383").bits() ==
          (u128{0x7ffe} << 112 | ((u128{1} << 112) - 1)));
}

TEST_CASE("f128 reports parse status") {
    using enum float_parse_status;
    CHECK(f128::parse("1e5000").status == TOO_LARGE);
    CHECK(f128::parse("1e-5000").status == TOO_SMALL);
    CHECK(f128::parse("0x1p99999999999999").status == TOO_LARGE);
    CHECK(f128::parse("0x1p-99999999999999").status == TOO_SMALL);
    CHECK(f128::parse("1e39", float_format::SINGLE).status == TOO_LARGE);
    CHECK(f128::parse("0.0").status == OK);
    CHECK(f128::parse("1e").status == MALFORMED);
    CHECK(f128::parse("1.2.3").status == MALFORMED);
    CHECK(f128::parse("0x1.8e3").status == OK); // `e` is a hex digit
    CHECK(f128::parse("").status == MALFORMED);
    CHECK(f128::parse(".").status == MALFORMED);
}

TEST_CASE("f128 prints doubles exactly like fmt") {
    double_source source;
    for (i32 i{0}; i < 20'000; ++i) {
        const auto value{source.next()};
        CHECK(f128::from_f64(value).to_string(float_format::DOUBLE) == fmt::format("{}", value));
    }
    for (const f64 value :
         {0.0, -0.0, 1.0, 100000.0, 1e15, 1e16, 1e-4, 1e-5, 0.1, 1e300, 123.456}) {
        CHECK(f128::from_f64(value).to_string(float_format::DOUBLE) == fmt::format("{}", value));
    }
}

TEST_CASE("f128 prints the shortest text that reads back") {
    double_source source;
    for (i32 i{0}; i < 2'000; ++i) {
        // A quad with random low bits below double precision
        const auto value{add(f128::from_f64(source.next()),
                             f128::from_f64(0x1p-60).round_to(float_format::QUAD))};
        const auto text{value.to_string()};
        INFO(text);
        CHECK(parse_value(text).bits() == value.bits());
    }
    CHECK(f128::parse("0.1").value.to_string() == "0.1");
    CHECK(f128::parse("1e4932").value.to_string() == "1e+4932");
    CHECK(f128::quiet_nan().to_string() == "nan");
    CHECK((-f128::infinity()).to_string() == "-inf");
}

TEST_CASE("f128 converts to and from integers") {
    const auto max_i128{std::numeric_limits<i128>::max()};
    const auto min_i128{std::numeric_limits<i128>::min()};
    CHECK(f128::from_int(-5).to_f64() == -5.0);
    CHECK(UNWRAP(f128::from_f64(-7.9).to_int()) == -7);
    CHECK(UNWRAP(f128::from_f64(7.9).to_uint()) == 7);
    CHECK(UNWRAP(f128::from_f64(-0.5).to_uint()) == 0);
    CHECK(!f128::from_f64(-1.5).to_uint());
    CHECK(UNWRAP(f128::from_int(min_i128).to_int()) == min_i128);
    // `i128` max needs 127 significand bits, so it rounds up to 2^127 and no longer fits
    CHECK(!f128::from_int(max_i128).to_int());
    CHECK(UNWRAP(f128::from_uint(~u128{0} >> 15).to_uint()) == (~u128{0} >> 15));
    CHECK(!f128::from_uint(~u128{0}).to_uint());
    CHECK(!f128::quiet_nan().to_int());
    CHECK(f128::from_uint(u128{1} << 64, float_format::HALF).is_infinite());
}

TEST_CASE("f128 compares by IEEE rules") {
    const auto nan{f128::quiet_nan()};
    const auto one{f128::from_f64(1.0)};
    CHECK(f128::zero(true) == f128::zero());
    CHECK(!(nan == nan));
    CHECK((nan <=> one) == std::partial_ordering::unordered);
    CHECK(-one < one);
    CHECK(f128::from_f64(-2.0) < -one);
    CHECK(one < f128::infinity());
    CHECK(-f128::infinity() < f128::from_f64(-1e300));
}

TEST_CASE("f128 keeps signed zeros and special values by IEEE rules") {
    const auto zero{f128::zero()};
    const auto negative_zero{f128::zero(true)};
    const auto one{f128::from_f64(1.0)};
    CHECK(add(negative_zero, negative_zero).is_negative());
    CHECK(!add(negative_zero, zero).is_negative());
    CHECK(!subtract(one, one).is_negative());
    CHECK(divide(one, negative_zero) == -f128::infinity());
    CHECK(divide(zero, zero).is_nan());
    CHECK(multiply(f128::infinity(), zero).is_nan());
    CHECK(add(f128::infinity(), -f128::infinity()).is_nan());
    CHECK(fused_multiply_add(f128::infinity(), zero, one).is_nan());
    CHECK(f128::from_f64(-2.5).trunc().to_f64() == -2.0);
    CHECK(f128::from_f64(-0.5).trunc().is_negative());
}

} // namespace ghoti::tests
