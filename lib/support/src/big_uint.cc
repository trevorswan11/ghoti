#include "support/big_uint.hh"

#include <algorithm>
#include <bit>
#include <compare>
#include <string>
#include <utility>
#include <vector>

#include <stdx/types.hh>

#include "support/int128.hh"

namespace ghoti {

namespace {

[[nodiscard]] constexpr auto wide(u64 v) noexcept -> u128 { return u128{v}; }

} // namespace

big_uint::big_uint(u128 value) : limbs_{value.low, value.high} { trim(); }

auto big_uint::power(u32 base, u64 exponent) -> big_uint {
    big_uint result{u128{1}};
    big_uint square{u128{base}};
    while (exponent != 0) {
        if ((exponent & 1U) != 0) { result = result * square; }
        exponent >>= 1U;
        if (exponent != 0) { square = square * square; }
    }
    return result;
}

auto big_uint::from_limbs(std::vector<u64> limbs) -> big_uint {
    big_uint result;
    result.limbs_ = std::move(limbs);
    result.trim();
    return result;
}

auto big_uint::bit_length() const noexcept -> u64 {
    if (limbs_.empty()) { return 0; }
    return ((limbs_.size() - 1) * 64) + static_cast<u64>(std::bit_width(limbs_.back()));
}

auto big_uint::bit(u64 index) const noexcept -> bool {
    const auto limb{index / 64};
    return limb < limbs_.size() && ((limbs_[limb] >> (index % 64)) & 1U) != 0;
}

auto big_uint::any_below(u64 count) const noexcept -> bool {
    const auto whole_limbs{std::min<u64>(count / 64, limbs_.size())};
    for (u64 i{0}; i < whole_limbs; ++i) {
        if (limbs_[i] != 0) { return true; }
    }
    if (whole_limbs == limbs_.size() || count % 64 == 0) { return false; }
    return (limbs_[whole_limbs] & ((u64{1} << (count % 64)) - 1U)) != 0;
}

auto big_uint::low_u128() const noexcept -> u128 {
    const u64 low{limbs_.empty() ? 0 : limbs_[0]};
    const u64 high{limbs_.size() < 2 ? 0 : limbs_[1]};
    return (wide(high) << 64) | wide(low);
}

auto big_uint::shift_left(u64 count) -> void {
    if (limbs_.empty() || count == 0) { return; }
    const auto       limb_shift{count / 64};
    const auto       bit_shift{count % 64};
    std::vector<u64> shifted(limbs_.size() + limb_shift + 1, 0);
    for (usize i{0}; i < limbs_.size(); ++i) {
        const u128 moved{wide(limbs_[i]) << bit_shift};
        shifted[i + limb_shift] |= moved.low;
        shifted[i + limb_shift + 1] |= moved.high;
    }
    limbs_ = std::move(shifted);
    trim();
}

auto big_uint::shift_right(u64 count) -> void {
    const auto limb_shift{count / 64};
    if (limb_shift >= limbs_.size()) {
        limbs_.clear();
        return;
    }
    const auto       bit_shift{count % 64};
    std::vector<u64> shifted(limbs_.size() - limb_shift);
    for (usize i{0}; i < shifted.size(); ++i) {
        const u64 low{limbs_[i + limb_shift]};
        const u64 high{i + limb_shift + 1 < limbs_.size() ? limbs_[i + limb_shift + 1] : 0};
        shifted[i] = (((wide(high) << 64) | wide(low)) >> bit_shift).low;
    }
    limbs_ = std::move(shifted);
    trim();
}

auto big_uint::keep_low(u64 count) -> void {
    const auto kept_limbs{(count + 63) / 64};
    if (limbs_.size() > kept_limbs) { limbs_.resize(kept_limbs); }
    if (count % 64 != 0 && limbs_.size() == kept_limbs) {
        limbs_.back() &= (u64{1} << (count % 64)) - 1U;
    }
    trim();
}

auto big_uint::add(const big_uint& other) -> void {
    limbs_.resize(std::max(limbs_.size(), other.limbs_.size()) + 1, 0);
    u64 carry{0};
    for (usize i{0}; i < limbs_.size(); ++i) {
        const u128 sum{wide(limbs_[i]) + wide(i < other.limbs_.size() ? other.limbs_[i] : 0) +
                       wide(carry)};
        limbs_[i] = sum.low;
        carry     = sum.high;
    }
    trim();
}

auto big_uint::subtract(const big_uint& other) -> void {
    u64 borrow{0};
    for (usize i{0}; i < limbs_.size(); ++i) {
        const u64  rhs{i < other.limbs_.size() ? other.limbs_[i] : 0};
        const bool needs_borrow{limbs_[i] < rhs || (limbs_[i] == rhs && borrow != 0)};
        limbs_[i] = limbs_[i] - rhs - borrow;
        borrow    = needs_borrow ? 1 : 0;
    }
    trim();
}

auto big_uint::multiply_add_small(u32 factor, u32 addend) -> void {
    u64 carry{addend};
    for (auto& limb : limbs_) {
        const u128 product{(wide(limb) * wide(factor)) + wide(carry)};
        limb  = product.low;
        carry = product.high;
    }
    if (carry != 0) { limbs_.emplace_back(carry); }
    trim();
}

auto big_uint::divide_small(u32 divisor) -> u32 {
    u64 remainder{0};
    for (usize i{limbs_.size()}; i-- > 0;) {
        const u128 current{(wide(remainder) << 64) | wide(limbs_[i])};
        limbs_[i] = (current / wide(divisor)).low;
        remainder = (current % wide(divisor)).low;
    }
    trim();
    return static_cast<u32>(remainder);
}

auto operator*(const big_uint& lhs, const big_uint& rhs) -> big_uint {
    if (lhs.is_zero() || rhs.is_zero()) { return {}; }
    big_uint product;
    product.limbs_.assign(lhs.limbs_.size() + rhs.limbs_.size(), 0);
    for (usize i{0}; i < lhs.limbs_.size(); ++i) {
        u64 carry{0};
        for (usize j{0}; j < rhs.limbs_.size(); ++j) {
            const u128 cell{(wide(lhs.limbs_[i]) * wide(rhs.limbs_[j])) +
                            wide(product.limbs_[i + j]) + wide(carry)};
            product.limbs_[i + j] = cell.low;
            carry                 = cell.high;
        }
        product.limbs_[i + rhs.limbs_.size()] = carry;
    }
    product.trim();
    return product;
}

auto big_uint::divide(big_uint& numerator, const big_uint& divisor) -> big_uint {
    const auto numerator_bits{numerator.bit_length()};
    const auto divisor_bits{divisor.bit_length()};
    if (numerator_bits < divisor_bits) { return {}; }

    // Only the top `numerator_bits - divisor_bits + 1` positions can hold quotient bits
    const auto quotient_bits{numerator_bits - divisor_bits + 1};
    big_uint   remainder{numerator};
    remainder.shift_right(quotient_bits);
    big_uint quotient;
    quotient.limbs_.assign((quotient_bits + 63) / 64, 0);
    for (u64 i{quotient_bits}; i-- > 0;) {
        remainder.shift_left_one(numerator.bit(i));
        if (remainder >= divisor) {
            remainder.subtract(divisor);
            quotient.limbs_[i / 64] |= u64{1} << (i % 64);
        }
    }
    quotient.trim();
    numerator = std::move(remainder);
    return quotient;
}

auto big_uint::to_decimal() const -> std::string {
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

auto big_uint::operator<=>(const big_uint& other) const noexcept -> std::strong_ordering {
    if (limbs_.size() != other.limbs_.size()) { return limbs_.size() <=> other.limbs_.size(); }
    for (usize i{limbs_.size()}; i-- > 0;) {
        if (limbs_[i] != other.limbs_[i]) { return limbs_[i] <=> other.limbs_[i]; }
    }
    return std::strong_ordering::equal;
}

auto big_uint::shift_left_one(bool low_bit) -> void {
    u64 carry{low_bit ? 1U : 0U};
    for (auto& limb : limbs_) {
        const u64 next_carry{limb >> 63};
        limb  = (limb << 1) | carry;
        carry = next_carry;
    }
    if (carry != 0) { limbs_.emplace_back(carry); }
}

auto big_uint::trim() -> void {
    while (!limbs_.empty() && limbs_.back() == 0) { limbs_.pop_back(); }
}

} // namespace ghoti
