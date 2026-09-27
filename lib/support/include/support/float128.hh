#pragma once

#include <compare>
#include <string>
#include <string_view>

#include <fmt/base.h>
#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "support/int128.hh"

namespace ghoti {

// The binary floating-point formats a compile-time float can be rounded to
enum class float_format : u8 {
    HALF,
    SINGLE,
    DOUBLE,
    X87,
    QUAD,
};

struct float_format_info {
    u32 precision; // significand bits, counting the leading one
    i32 min_exponent;
    i32 max_exponent;
    u32 storage_bits;
};

[[nodiscard]] auto format_info(float_format format) noexcept -> float_format_info;

enum class float_parse_status : u8 {
    OK,
    TOO_LARGE, // rounded to infinity
    TOO_SMALL, // a nonzero value rounded to zero
    MALFORMED,
};

struct float_parse_result;

// An IEEE-754 binary128 value. Operations compute the exact result and round it once, to nearest
// with ties to even, into the requested format.
class float128 {
  public:
    constexpr float128() noexcept = default;

    [[nodiscard]] static auto from_bits(u128 bits) noexcept -> float128;
    [[nodiscard]] static auto from_f64(f64 value) -> float128;
    [[nodiscard]] static auto from_int(i128 value, float_format format = float_format::QUAD)
        -> float128;
    [[nodiscard]] static auto from_uint(u128 value, float_format format = float_format::QUAD)
        -> float128;
    // Reads the `format` encoding held in the low bits of `bits`
    [[nodiscard]] static auto decode(u128 bits, float_format format) -> float128;
    [[nodiscard]] static auto infinity(bool negative = false) noexcept -> float128;
    [[nodiscard]] static auto quiet_nan() noexcept -> float128;
    [[nodiscard]] static auto zero(bool negative = false) noexcept -> float128;

    // Decimal (`1.5e3`) or hex (`0x1.8p3`) text with an optional sign and no digit separators
    [[nodiscard]] static auto parse(std::string_view text, float_format format = float_format::QUAD)
        -> float_parse_result;

    [[nodiscard]] auto bits() const noexcept -> u128 { return bits_; }
    // The `format` encoding of this value after rounding to it
    [[nodiscard]] auto encode(float_format format) const -> u128;
    [[nodiscard]] auto round_to(float_format format) const -> float128;
    [[nodiscard]] auto to_f64() const -> f64;
    [[nodiscard]] auto trunc() const -> float128;
    // Truncates toward zero; none for NaN, infinity, or a result the integer type can't hold
    [[nodiscard]] auto to_int() const -> stdx::option<i128>;
    [[nodiscard]] auto to_uint() const -> stdx::option<u128>;
    // The shortest decimal that reads back as this value once rounded to `format`
    [[nodiscard]] auto to_string(float_format format = float_format::QUAD) const -> std::string;

    [[nodiscard]] auto is_nan() const noexcept -> bool;
    [[nodiscard]] auto is_infinite() const noexcept -> bool;
    [[nodiscard]] auto is_finite() const noexcept -> bool;
    [[nodiscard]] auto is_zero() const noexcept -> bool;
    [[nodiscard]] auto is_negative() const noexcept -> bool;

    [[nodiscard]] auto operator-() const noexcept -> float128;
    [[nodiscard]] auto abs() const noexcept -> float128;

    // IEEE comparison: NaN is unordered and `-0 == +0`
    [[nodiscard]] auto operator==(const float128& other) const noexcept -> bool;
    [[nodiscard]] auto operator<=>(const float128& other) const noexcept -> std::partial_ordering;

  private:
    u128 bits_{};
};

struct float_parse_result {
    float128           value;
    float_parse_status status;
};

[[nodiscard]] auto add(float128 lhs, float128 rhs, float_format format = float_format::QUAD)
    -> float128;
[[nodiscard]] auto subtract(float128 lhs, float128 rhs, float_format format = float_format::QUAD)
    -> float128;
[[nodiscard]] auto multiply(float128 lhs, float128 rhs, float_format format = float_format::QUAD)
    -> float128;
[[nodiscard]] auto divide(float128 lhs, float128 rhs, float_format format = float_format::QUAD)
    -> float128;
// `a * b + c` with a single rounding
[[nodiscard]] auto
fused_multiply_add(float128 a, float128 b, float128 c, float_format format = float_format::QUAD)
    -> float128;

[[nodiscard]] inline auto operator+(float128 lhs, float128 rhs) -> float128 {
    return add(lhs, rhs);
}
[[nodiscard]] inline auto operator-(float128 lhs, float128 rhs) -> float128 {
    return subtract(lhs, rhs);
}
[[nodiscard]] inline auto operator*(float128 lhs, float128 rhs) -> float128 {
    return multiply(lhs, rhs);
}
[[nodiscard]] inline auto operator/(float128 lhs, float128 rhs) -> float128 {
    return divide(lhs, rhs);
}

} // namespace ghoti

template <> struct fmt::formatter<ghoti::float128> {
    static constexpr auto parse(format_parse_context& ctx) noexcept { return ctx.begin(); }

    static auto format(const ghoti::float128& f, format_context& ctx) {
        return fmt::format_to(ctx.out(), "{}", f.to_string());
    }
};
