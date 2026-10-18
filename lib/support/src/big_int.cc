#include <algorithm>
#include <compare>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "support/big_int.hh"
#include "support/big_uint.hh"
#include "support/int128.hh"

namespace ghoti {

namespace {

[[nodiscard]] auto word_count(u64 width) noexcept -> usize {
    return static_cast<usize>((width + 63) / 64);
}

// Clears every bit at or above `width`
auto mask_to_width(std::vector<u64>& words, u64 width) -> void {
    if (width % 64 != 0 && !words.empty()) { words.back() &= (u64{1} << (width % 64)) - 1; }
}

[[nodiscard]] auto magnitude_words(const big_uint& magnitude, usize count) -> std::vector<u64> {
    auto words{magnitude.limbs()};
    words.resize(count, 0);
    return words;
}

// Two's complement negation in place, modulo the words' full width
auto negate_words(std::vector<u64>& words) -> void {
    bool carry{true};
    for (auto& word : words) {
        word = ~word;
        if (carry) {
            ++word;
            carry = word == 0;
        }
    }
}

template <typename Op>
[[nodiscard]] auto bitwise(const big_int& lhs, const big_int& rhs, Op op) -> big_int {
    const auto       width{std::max(lhs.signed_bits(), rhs.signed_bits()) + 1};
    auto             l{lhs.to_words(width)};
    const auto       r{rhs.to_words(width)};
    std::vector<u64> out(l.size());
    for (usize i{0}; i < l.size(); ++i) { out[i] = op(l[i], r[i]); }
    mask_to_width(out, width);
    return big_int::from_words(out, width, true);
}

[[nodiscard]] auto add_magnitudes(bool negative, const big_uint& l, const big_uint& r) -> big_int {
    big_uint sum{l};
    sum.add(r);
    return big_int{negative, std::move(sum)};
}

// `l - r` for magnitudes of opposite-signed operands, keeping the larger one's sign
[[nodiscard]] auto subtract_magnitudes(bool l_negative, const big_uint& l, const big_uint& r)
    -> big_int {
    if (l >= r) {
        big_uint diff{l};
        diff.subtract(r);
        return big_int{l_negative, std::move(diff)};
    }
    big_uint diff{r};
    diff.subtract(l);
    return big_int{!l_negative, std::move(diff)};
}

} // namespace

big_int::big_int(i128 value)
    : negative_{value < 0},
      magnitude_{value < 0 ? u128{0} - static_cast<u128>(value) : static_cast<u128>(value)} {}

big_int::big_int(bool negative, big_uint magnitude)
    : negative_{negative && !magnitude.is_zero()}, magnitude_{std::move(magnitude)} {}

auto big_int::parse(std::string_view digits, u32 base) -> stdx::option<big_int> {
    if (digits.empty()) { return stdx::none; }
    big_uint value;
    for (const char c : digits) {
        u32 digit{0};
        if (c >= '0' && c <= '9') {
            digit = static_cast<u32>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<u32>(c - 'a') + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<u32>(c - 'A') + 10;
        } else {
            return stdx::none;
        }
        if (digit >= base) { return stdx::none; }
        value.multiply_add_small(base, digit);
    }
    return big_int{false, std::move(value)};
}

auto big_int::to_i128() const noexcept -> stdx::option<i128> {
    if (!fits(128, true)) { return stdx::none; }
    const auto low{magnitude_.low_u128()};
    return negative_ ? static_cast<i128>(u128{0} - low) : static_cast<i128>(low);
}

auto big_int::to_u128() const noexcept -> stdx::option<u128> {
    if (!fits(128, false)) { return stdx::none; }
    return magnitude_.low_u128();
}

auto big_int::signed_bits() const noexcept -> u64 {
    if (!negative_) { return magnitude_.bit_length() + 1; }
    // -m needs one more bit than m - 1
    big_uint below{magnitude_};
    below.subtract(big_uint{u128{1}});
    return below.bit_length() + 1;
}

auto big_int::unsigned_bits() const noexcept -> stdx::option<u64> {
    if (negative_) { return stdx::none; }
    return magnitude_.bit_length();
}

auto big_int::fits(u64 bits, bool is_signed) const noexcept -> bool {
    if (is_signed) { return signed_bits() <= bits; }
    return !negative_ && magnitude_.bit_length() <= bits;
}

auto big_int::to_words(u64 width) const -> std::vector<u64> {
    auto words{magnitude_words(magnitude_, word_count(width))};
    if (negative_) { negate_words(words); }
    mask_to_width(words, width);
    return words;
}

auto big_int::from_words(const std::vector<u64>& words, u64 width, bool is_signed) -> big_int {
    auto value{words};
    value.resize(word_count(width), 0);
    mask_to_width(value, width);
    const bool negative{is_signed && width != 0 &&
                        ((value.back() >> ((width - 1) % 64)) & 1U) != 0};
    if (negative) {
        negate_words(value);
        mask_to_width(value, width);
    }
    return big_int{negative, big_uint::from_limbs(std::move(value))};
}

auto big_int::to_string() const -> std::string {
    return negative_ ? "-" + magnitude_.to_decimal() : magnitude_.to_decimal();
}

auto big_int::operator-() const -> big_int { return big_int{!negative_, magnitude_}; }

auto big_int::operator~() const -> big_int { return -*this - big_int{i128{1}}; }

auto big_int::abs() const -> big_int { return big_int{false, magnitude_}; }

auto operator+(const big_int& lhs, const big_int& rhs) -> big_int {
    if (lhs.negative_ == rhs.negative_) {
        return add_magnitudes(lhs.negative_, lhs.magnitude_, rhs.magnitude_);
    }
    return subtract_magnitudes(lhs.negative_, lhs.magnitude_, rhs.magnitude_);
}

auto operator-(const big_int& lhs, const big_int& rhs) -> big_int { return lhs + -rhs; }

auto operator*(const big_int& lhs, const big_int& rhs) -> big_int {
    return big_int{lhs.negative_ != rhs.negative_, lhs.magnitude_ * rhs.magnitude_};
}

auto operator/(const big_int& lhs, const big_int& rhs) -> big_int {
    big_uint remainder{lhs.magnitude_};
    auto     quotient{big_uint::divide(remainder, rhs.magnitude_)};
    return big_int{lhs.negative_ != rhs.negative_, std::move(quotient)};
}

auto operator%(const big_int& lhs, const big_int& rhs) -> big_int {
    big_uint remainder{lhs.magnitude_};
    DISCARD(big_uint::divide(remainder, rhs.magnitude_));
    return big_int{lhs.negative_, std::move(remainder)};
}

auto operator&(const big_int& lhs, const big_int& rhs) -> big_int {
    return bitwise(lhs, rhs, [](u64 l, u64 r) { return l & r; });
}

auto operator|(const big_int& lhs, const big_int& rhs) -> big_int {
    return bitwise(lhs, rhs, [](u64 l, u64 r) { return l | r; });
}

auto operator^(const big_int& lhs, const big_int& rhs) -> big_int {
    return bitwise(lhs, rhs, [](u64 l, u64 r) { return l ^ r; });
}

auto big_int::shl(u64 amount) const -> big_int {
    big_uint shifted{magnitude_};
    shifted.shift_left(amount);
    return big_int{negative_, std::move(shifted)};
}

auto big_int::shr(u64 amount) const -> big_int {
    big_uint shifted{magnitude_};
    shifted.shift_right(amount);
    if (negative_ && magnitude_.any_below(amount)) { shifted.add(big_uint{u128{1}}); }
    return big_int{negative_, std::move(shifted)};
}

auto big_int::operator<=>(const big_int& other) const noexcept -> std::strong_ordering {
    if (negative_ != other.negative_) {
        return negative_ ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    const auto by_magnitude{magnitude_ <=> other.magnitude_};
    return negative_ ? 0 <=> by_magnitude : by_magnitude;
}

} // namespace ghoti
