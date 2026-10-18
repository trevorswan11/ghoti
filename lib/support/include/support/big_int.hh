#pragma once

#include <compare>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/base.h>
#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "support/big_uint.hh"
#include "support/int128.hh"

namespace ghoti {

// An arbitrary-precision signed integer, as sign and magnitude. Bitwise operators act on the
// infinite two's complement form, so `-1 & x == x` like on any fixed width
class big_int {
  public:
    big_int() = default;
    explicit big_int(i128 value);
    explicit big_int(u128 value) : magnitude_{value} {}
    big_int(bool negative, big_uint magnitude);

    // Digits in `base` (2, 8, 10, or 16) with no prefix, sign, or separators
    [[nodiscard]] static auto parse(std::string_view digits, u32 base) -> stdx::option<big_int>;

    [[nodiscard]] auto is_zero() const noexcept -> bool { return magnitude_.is_zero(); }
    [[nodiscard]] auto is_negative() const noexcept -> bool { return negative_; }
    [[nodiscard]] auto magnitude() const noexcept -> const big_uint& { return magnitude_; }

    [[nodiscard]] auto to_i128() const noexcept -> stdx::option<i128>;
    [[nodiscard]] auto to_u128() const noexcept -> stdx::option<u128>;

    // The fewest bits that hold the value as a signed, or an unsigned, two's complement integer
    [[nodiscard]] auto signed_bits() const noexcept -> u64;
    [[nodiscard]] auto unsigned_bits() const noexcept -> stdx::option<u64>;
    [[nodiscard]] auto fits(u64 bits, bool is_signed) const noexcept -> bool;

    // The value modulo 2^`width`, as little-endian 64-bit words
    [[nodiscard]] auto to_words(u64 width) const -> std::vector<u64>;
    // The two's complement integer the lowest `width` bits of `words` spell
    [[nodiscard]] static auto from_words(const std::vector<u64>& words, u64 width, bool is_signed)
        -> big_int;

    [[nodiscard]] auto to_string() const -> std::string;

    [[nodiscard]] auto operator-() const -> big_int;
    [[nodiscard]] auto operator~() const -> big_int;
    [[nodiscard]] auto abs() const -> big_int;

    friend auto operator+(const big_int& lhs, const big_int& rhs) -> big_int;
    friend auto operator-(const big_int& lhs, const big_int& rhs) -> big_int;
    friend auto operator*(const big_int& lhs, const big_int& rhs) -> big_int;
    // Truncates toward zero; the divisor must be nonzero
    friend auto operator/(const big_int& lhs, const big_int& rhs) -> big_int;
    // Takes the dividend's sign; the divisor must be nonzero
    friend auto operator%(const big_int& lhs, const big_int& rhs) -> big_int;
    friend auto operator&(const big_int& lhs, const big_int& rhs) -> big_int;
    friend auto operator|(const big_int& lhs, const big_int& rhs) -> big_int;
    friend auto operator^(const big_int& lhs, const big_int& rhs) -> big_int;

    [[nodiscard]] auto shl(u64 amount) const -> big_int;
    // Floors like an arithmetic shift, so a negative value never rounds toward zero
    [[nodiscard]] auto shr(u64 amount) const -> big_int;

    [[nodiscard]] auto operator<=>(const big_int& other) const noexcept -> std::strong_ordering;
    [[nodiscard]] auto operator==(const big_int& other) const noexcept -> bool = default;

  private:
    bool     negative_{false};
    big_uint magnitude_;
};

} // namespace ghoti

template <> struct fmt::formatter<ghoti::big_int> {
    static constexpr auto parse(format_parse_context& ctx) noexcept { return ctx.begin(); }

    static auto format(const ghoti::big_int& i, format_context& ctx) {
        return fmt::format_to(ctx.out(), "{}", i.to_string());
    }
};
