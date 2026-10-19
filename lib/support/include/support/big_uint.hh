#pragma once

#include <compare>
#include <string>
#include <vector>

#include <stdx/types.hh>

#include "support/int128.hh"

namespace ghoti {

// An arbitrary-precision unsigned integer
class big_uint {
  public:
    big_uint() = default;
    explicit big_uint(u128 value);

    [[nodiscard]] static auto power(u32 base, u64 exponent) -> big_uint;

    // Little-endian 64-bit limbs, with no zero limb on top
    [[nodiscard]] static auto from_limbs(std::vector<u64> limbs) -> big_uint;
    [[nodiscard]] auto        limbs() const noexcept -> const std::vector<u64>& { return limbs_; }

    [[nodiscard]] auto is_zero() const noexcept -> bool { return limbs_.empty(); }
    [[nodiscard]] auto bit_length() const noexcept -> u64;
    [[nodiscard]] auto bit(u64 index) const noexcept -> bool;
    // Whether any of the lowest `count` bits is set
    [[nodiscard]] auto any_below(u64 count) const noexcept -> bool;
    [[nodiscard]] auto low_u128() const noexcept -> u128;

    auto shift_left(u64 count) -> void;
    auto shift_right(u64 count) -> void;
    // Keeps only the lowest `count` bits
    auto keep_low(u64 count) -> void;

    auto add(const big_uint& other) -> void;
    // Requires `*this >= other`
    auto subtract(const big_uint& other) -> void;
    auto multiply_add_small(u32 factor, u32 addend) -> void;
    // Divides in place, returning the remainder
    auto divide_small(u32 divisor) -> u32;

    friend auto operator*(const big_uint& lhs, const big_uint& rhs) -> big_uint;

    // Long division, returning the quotient and leaving the remainder in `numerator`
    [[nodiscard]] static auto divide(big_uint& numerator, const big_uint& divisor) -> big_uint;

    [[nodiscard]] auto to_decimal() const -> std::string;

    [[nodiscard]] auto operator<=>(const big_uint& other) const noexcept -> std::strong_ordering;
    [[nodiscard]] auto operator==(const big_uint& other) const noexcept -> bool = default;

  private:
    auto shift_left_one(bool low_bit) -> void;
    auto trim() -> void;

  private:
    std::vector<u64> limbs_;
};

} // namespace ghoti
