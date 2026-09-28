#include <array>
#include <random>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <fmt/format.h>
#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/APSInt.h>
#include <llvm/Support/Error.h>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "support/float128.hh"
#include "support/int128.hh"

namespace ghoti::tests {

namespace {

constexpr std::array ALL_FORMATS{float_format::HALF,
                                 float_format::SINGLE,
                                 float_format::DOUBLE,
                                 float_format::X87,
                                 float_format::QUAD};

[[nodiscard]] auto semantics_of(float_format format) -> const llvm::fltSemantics& {
    switch (format) {
    case float_format::HALF:   return llvm::APFloat::IEEEhalf();
    case float_format::SINGLE: return llvm::APFloat::IEEEsingle();
    case float_format::DOUBLE: return llvm::APFloat::IEEEdouble();
    case float_format::X87:    return llvm::APFloat::x87DoubleExtended();
    case float_format::QUAD:   return llvm::APFloat::IEEEquad();
    }
    return llvm::APFloat::IEEEquad();
}

[[nodiscard]] auto to_apint(u128 bits, u32 width) -> llvm::APInt {
    const std::array<u64, 2> words{bits.low, bits.high};
    return llvm::APInt{width, words};
}

[[nodiscard]] auto to_apfloat(f128 value, float_format format) -> llvm::APFloat {
    return llvm::APFloat{semantics_of(format),
                         to_apint(value.encode(format), format_info(format).storage_bits)};
}

[[nodiscard]] auto apfloat_from_text(std::string_view text, float_format format) -> llvm::APFloat {
    llvm::APFloat value{semantics_of(format)};
    auto          status{value.convertFromString(text, llvm::APFloat::rmNearestTiesToEven)};
    REQUIRE(static_cast<bool>(status));
    return value;
}

// Bit-exact agreement, treating every NaN as equal
[[nodiscard]] auto agrees(const llvm::APFloat& expected, f128 actual, float_format format) -> bool {
    if (expected.isNaN()) { return actual.round_to(format).is_nan(); }
    return expected.bitcastToAPInt() ==
           to_apint(actual.encode(format), format_info(format).storage_bits);
}

class value_source {
  public:
    // Random significand bits at a random exponent reaching just past the format's range
    auto next(float_format format) -> f128 {
        const auto                         info{format_info(format)};
        std::uniform_int_distribution<i32> exponent{info.min_exponent - 120, info.max_exponent + 1};
        const auto                         text{fmt::format("{}0x{:016x}{:016x}p{}",
                                    (bits_(engine_) & 1) != 0 ? "-" : "",
                                    bits_(engine_),
                                    bits_(engine_),
                                    exponent(engine_) - 127)};
        return f128::parse(text, format).value;
    }

    auto next_int() -> i128 {
        const u128 bits{(u128{bits_(engine_)} << 64) | u128{bits_(engine_)}};
        const auto width{static_cast<u32>(bits_(engine_) % 128)};
        return static_cast<i128>(bits >> width);
    }

    auto next_decimal_text(bool hex) -> std::string {
        std::uniform_int_distribution<u32> digit_count{1, 40};
        std::uniform_int_distribution<i32> exponent{hex ? -16'600 : -5'000, hex ? 16'600 : 5'000};
        std::string                        text{(bits_(engine_) & 1) != 0 ? "-" : ""};
        if (hex) { text += "0x"; }
        const auto count{digit_count(engine_)};
        const auto point{bits_(engine_) % (count + 1)};
        for (u32 i{0}; i < count; ++i) {
            if (i == point && i != 0) { text += '.'; }
            text += "0123456789abcdef"[bits_(engine_) % (hex ? 16 : 10)];
        }
        text += fmt::format("{}{}", hex ? 'p' : 'e', exponent(engine_));
        return text;
    }

  private:
    std::mt19937_64                    engine_{0xf10a7};
    std::uniform_int_distribution<u64> bits_;
};

using apfloat_op  = llvm::APFloat::opStatus (llvm::APFloat::*)(const llvm::APFloat&,
                                                              llvm::APFloat::roundingMode);
using float128_op = f128 (*)(f128, f128, float_format);

} // namespace

TEST_CASE("f128 arithmetic matches APFloat in every format") {
    constexpr auto                                          RNE{llvm::APFloat::rmNearestTiesToEven};
    const std::array<std::pair<apfloat_op, float128_op>, 4> ops{{
        {&llvm::APFloat::add, &add},
        {&llvm::APFloat::subtract, &subtract},
        {&llvm::APFloat::multiply, &multiply},
        {&llvm::APFloat::divide, &divide},
    }};

    value_source source;
    const auto   format{GENERATE(from_range(ALL_FORMATS))};
    for (i32 i{0}; i < 3'000; ++i) {
        const auto a{source.next(format)};
        const auto b{source.next(format)};
        const auto c{source.next(format)};

        for (const auto& [apfloat_fn, float128_fn] : ops) {
            auto expected{to_apfloat(a, format)};
            DISCARD((expected.*apfloat_fn)(to_apfloat(b, format), RNE));
            CHECK(agrees(expected, float128_fn(a, b, format), format));
        }
        auto expected_fma{to_apfloat(a, format)};
        DISCARD(expected_fma.fusedMultiplyAdd(to_apfloat(b, format), to_apfloat(c, format), RNE));
        CHECK(agrees(expected_fma, fused_multiply_add(a, b, c, format), format));
    }
}

TEST_CASE("f128 narrowing and widening match APFloat") {
    const auto   format{GENERATE(from_range(ALL_FORMATS))};
    value_source source;
    for (i32 i{0}; i < 5'000; ++i) {
        const auto wide{source.next(float_format::QUAD)};
        auto       expected{to_apfloat(wide, float_format::QUAD)};
        bool       loses_info{false};
        DISCARD(expected.convert(
            semantics_of(format), llvm::APFloat::rmNearestTiesToEven, &loses_info));
        CHECK(agrees(expected, wide.round_to(format), format));

        // Decoding a narrow encoding back is exact
        const auto narrow{wide.round_to(format)};
        const auto decoded{f128::decode(narrow.encode(format), format)};
        CHECK((decoded.is_nan() ? narrow.is_nan() : decoded.bits() == narrow.bits()));
    }
}

TEST_CASE("f128 parses decimal and hex text like APFloat") {
    const bool   hex{GENERATE(true, false)};
    const auto   format{GENERATE(from_range(ALL_FORMATS))};
    value_source source;
    for (i32 i{0}; i < 1'500; ++i) {
        const auto text{source.next_decimal_text(hex)};
        const auto expected{apfloat_from_text(text, format)};
        const auto parsed{f128::parse(text, format)};
        REQUIRE(parsed.status != float_parse_status::MALFORMED);
        CHECK(agrees(expected, parsed.value, format));
    }
}

TEST_CASE("f128 shortest text reads back in every format") {
    const auto   format{GENERATE(from_range(ALL_FORMATS))};
    value_source source;
    for (i32 i{0}; i < 300; ++i) {
        const auto value{source.next(format)};
        const auto text{value.to_string(format)};
        if (value.is_nan() || value.is_infinite()) { continue; }
        const auto parsed{f128::parse(text, format).value};
        CHECK(parsed.bits() == value.bits());

        // APFloat reads the same text to the same value
        CHECK(agrees(apfloat_from_text(text, format), value, format));
    }
}

TEST_CASE("f128 integer conversions match APFloat") {
    const auto   format{GENERATE(from_range(ALL_FORMATS))};
    value_source source;
    for (i32 i{0}; i < 5'000; ++i) {
        const auto               integer{source.next_int()};
        const std::array<u64, 2> words{static_cast<u128>(integer).low,
                                       static_cast<u128>(integer).high};
        llvm::APFloat            expected{semantics_of(format)};
        DISCARD(expected.convertFromAPInt(
            llvm::APInt{128, words}, true, llvm::APFloat::rmNearestTiesToEven));
        CHECK(agrees(expected, f128::from_int(integer, format), format));

        const auto   value{source.next(float_format::QUAD)};
        llvm::APSInt truncated{128, false};
        bool         is_exact{false};
        const auto   status{to_apfloat(value, float_format::QUAD)
                              .convertToInteger(truncated, llvm::APFloat::rmTowardZero, &is_exact)};
        const auto   actual{value.to_int()};
        CHECK((status == llvm::APFloat::opInvalidOp) == !actual);
        if (actual) {
            CHECK(truncated.getLoBits(64).getZExtValue() == static_cast<u128>(*actual).low);
            CHECK(truncated.lshr(64).getZExtValue() == static_cast<u128>(*actual).high);
        }
    }
}

} // namespace ghoti::tests
