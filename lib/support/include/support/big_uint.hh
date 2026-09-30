#pragma once

#include <algorithm>
#include <bit>
#include <compare>
#include <string>
#include <utility>
#include <vector>

#include <stdx/types.hh>

#include "support/int128.hh"

namespace ghoti {

// An arbitrary-precision unsigned integer, just wide enough in features for exact float math
class big_uint {
  public:
    big_uint() = default;
    explicit big_uint(u128 value)
        : limbs_{static_cast<u32>(value.low),
                 static_cast<u32>(value.low >> 32),
                 static_cast<u32>(value.high),
                 static_cast<u32>(value.high >> 32)} {
        trim();
    }

    [[nodiscard]] static auto power(u32 base, u64 exponent) -> big_uint {
        big_uint result{u128{1}};
        big_uint square{u128{base}};
        while (exponent != 0) {
            if ((exponent & 1U) != 0) { result = result * square; }
            exponent >>= 1U;
            if (exponent != 0) { square = square * square; }
        }
        return result;
    }

    [[nodiscard]] auto is_zero() const noexcept -> bool { return limbs_.empty(); }

    [[nodiscard]] auto bit_length() const noexcept -> u64 {
        if (limbs_.empty()) { return 0; }
        return ((limbs_.size() - 1) * 32) + static_cast<u64>(std::bit_width(limbs_.back()));
    }

    [[nodiscard]] auto bit(u64 index) const noexcept -> bool {
        const auto limb{index / 32};
        return limb < limbs_.size() && ((limbs_[limb] >> (index % 32)) & 1U) != 0;
    }

    // Whether any of the lowest `count` bits is set
    [[nodiscard]] auto any_below(u64 count) const noexcept -> bool {
        const auto whole_limbs{std::min<u64>(count / 32, limbs_.size())};
        for (u64 i{0}; i < whole_limbs; ++i) {
            if (limbs_[i] != 0) { return true; }
        }
        if (whole_limbs == limbs_.size() || count % 32 == 0) { return false; }
        return (limbs_[whole_limbs] & ((1U << (count % 32)) - 1U)) != 0;
    }

    [[nodiscard]] auto low_u128() const noexcept -> u128 {
        u128 result{};
        for (usize i{std::min<usize>(limbs_.size(), 4)}; i-- > 0;) {
            result = (result << 32) | u128{limbs_[i]};
        }
        return result;
    }

    auto shift_left(u64 count) -> void {
        if (limbs_.empty() || count == 0) { return; }
        const auto       limb_shift{count / 32};
        const auto       bit_shift{count % 32};
        std::vector<u32> shifted(limbs_.size() + limb_shift + 1, 0);
        for (usize i{0}; i < limbs_.size(); ++i) {
            const u64 wide{static_cast<u64>(limbs_[i]) << bit_shift};
            shifted[i + limb_shift] |= static_cast<u32>(wide);
            shifted[i + limb_shift + 1] |= static_cast<u32>(wide >> 32);
        }
        limbs_ = std::move(shifted);
        trim();
    }

    auto shift_right(u64 count) -> void {
        const auto limb_shift{count / 32};
        if (limb_shift >= limbs_.size()) {
            limbs_.clear();
            return;
        }
        const auto       bit_shift{count % 32};
        std::vector<u32> shifted(limbs_.size() - limb_shift);
        for (usize i{0}; i < shifted.size(); ++i) {
            const u64 low{limbs_[i + limb_shift]};
            const u64 high{i + limb_shift + 1 < limbs_.size() ? limbs_[i + limb_shift + 1] : 0};
            shifted[i] = static_cast<u32>(((high << 32) | low) >> bit_shift);
        }
        limbs_ = std::move(shifted);
        trim();
    }

    // Keeps only the lowest `count` bits
    auto keep_low(u64 count) -> void {
        const auto kept_limbs{(count + 31) / 32};
        if (limbs_.size() > kept_limbs) { limbs_.resize(kept_limbs); }
        if (count % 32 != 0 && limbs_.size() == kept_limbs) {
            limbs_.back() &= (1U << (count % 32)) - 1U;
        }
        trim();
    }

    auto add(const big_uint& other) -> void {
        limbs_.resize(std::max(limbs_.size(), other.limbs_.size()) + 1, 0);
        u64 carry{0};
        for (usize i{0}; i < limbs_.size(); ++i) {
            const u64 sum{static_cast<u64>(limbs_[i]) +
                          (i < other.limbs_.size() ? other.limbs_[i] : 0) + carry};
            limbs_[i] = static_cast<u32>(sum);
            carry     = sum >> 32;
        }
        trim();
    }

    // Requires `*this >= other`
    auto subtract(const big_uint& other) -> void {
        i64 borrow{0};
        for (usize i{0}; i < limbs_.size(); ++i) {
            i64 diff{static_cast<i64>(limbs_[i]) -
                     (i < other.limbs_.size() ? static_cast<i64>(other.limbs_[i]) : 0) - borrow};
            borrow = diff < 0 ? 1 : 0;
            if (diff < 0) { diff += i64{1} << 32; }
            limbs_[i] = static_cast<u32>(diff);
        }
        trim();
    }

    auto multiply_add_small(u32 factor, u32 addend) -> void {
        u64 carry{addend};
        for (auto& limb : limbs_) {
            const u64 product{(static_cast<u64>(limb) * factor) + carry};
            limb  = static_cast<u32>(product);
            carry = product >> 32;
        }
        if (carry != 0) { limbs_.emplace_back(static_cast<u32>(carry)); }
        trim();
    }

    // Divides in place, returning the remainder
    auto divide_small(u32 divisor) -> u32 {
        u64 remainder{0};
        for (usize i{limbs_.size()}; i-- > 0;) {
            const u64 current{(remainder << 32) | limbs_[i]};
            limbs_[i] = static_cast<u32>(current / divisor);
            remainder = current % divisor;
        }
        trim();
        return static_cast<u32>(remainder);
    }

    [[nodiscard]] friend auto operator*(const big_uint& lhs, const big_uint& rhs) -> big_uint {
        if (lhs.is_zero() || rhs.is_zero()) { return {}; }
        big_uint product;
        product.limbs_.assign(lhs.limbs_.size() + rhs.limbs_.size(), 0);
        for (usize i{0}; i < lhs.limbs_.size(); ++i) {
            u64 carry{0};
            for (usize j{0}; j < rhs.limbs_.size(); ++j) {
                const u64 cell{(static_cast<u64>(lhs.limbs_[i]) * rhs.limbs_[j]) +
                               product.limbs_[i + j] + carry};
                product.limbs_[i + j] = static_cast<u32>(cell);
                carry                 = cell >> 32;
            }
            product.limbs_[i + rhs.limbs_.size()] = static_cast<u32>(carry);
        }
        product.trim();
        return product;
    }

    // Long division, returning the quotient and leaving the remainder in `numerator`
    [[nodiscard]] static auto divide(big_uint& numerator, const big_uint& divisor) -> big_uint {
        const auto numerator_bits{numerator.bit_length()};
        const auto divisor_bits{divisor.bit_length()};
        if (numerator_bits < divisor_bits) { return {}; }

        // Only the top `numerator_bits - divisor_bits + 1` positions can hold quotient bits
        const auto quotient_bits{numerator_bits - divisor_bits + 1};
        big_uint   remainder{numerator};
        remainder.shift_right(quotient_bits);
        big_uint quotient;
        quotient.limbs_.assign((quotient_bits + 31) / 32, 0);
        for (u64 i{quotient_bits}; i-- > 0;) {
            remainder.shift_left_one(numerator.bit(i));
            if (remainder >= divisor) {
                remainder.subtract(divisor);
                quotient.limbs_[i / 32] |= 1U << (i % 32);
            }
        }
        quotient.trim();
        numerator = std::move(remainder);
        return quotient;
    }

    [[nodiscard]] auto to_decimal() const -> std::string {
        if (is_zero()) { return "0"; }
        big_uint         rest{*this};
        std::vector<u32> chunks;
        while (!rest.is_zero()) { chunks.emplace_back(rest.divide_small(1'000'000'000)); }
        auto digits{std::to_string(chunks.back())};
        for (usize i{chunks.size() - 1}; i-- > 0;) {
            const auto chunk{std::to_string(chunks[i])};
            digits.append(9 - chunk.size(), '0');
            digits += chunk;
        }
        return digits;
    }

    [[nodiscard]] auto operator<=>(const big_uint& other) const noexcept -> std::strong_ordering {
        if (limbs_.size() != other.limbs_.size()) { return limbs_.size() <=> other.limbs_.size(); }
        for (usize i{limbs_.size()}; i-- > 0;) {
            if (limbs_[i] != other.limbs_[i]) { return limbs_[i] <=> other.limbs_[i]; }
        }
        return std::strong_ordering::equal;
    }
    [[nodiscard]] auto operator==(const big_uint& other) const noexcept -> bool = default;

  private:
    auto shift_left_one(bool low_bit) -> void {
        u32 carry{low_bit ? 1U : 0U};
        for (auto& limb : limbs_) {
            const u32 next_carry{limb >> 31};
            limb  = (limb << 1) | carry;
            carry = next_carry;
        }
        if (carry != 0) { limbs_.emplace_back(carry); }
    }

    auto trim() -> void {
        while (!limbs_.empty() && limbs_.back() == 0) { limbs_.pop_back(); }
    }

    std::vector<u32> limbs_;
};

} // namespace ghoti
