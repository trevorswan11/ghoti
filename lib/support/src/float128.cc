#include "support/float128.hh"

#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <stdx/option.hh>
#include <stdx/types.hh>

#include "support/int128.hh"

namespace ghoti {

namespace {

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
        if (carry != 0) { limbs_.push_back(static_cast<u32>(carry)); }
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
        while (!rest.is_zero()) { chunks.push_back(rest.divide_small(1'000'000'000)); }
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
        if (carry != 0) { limbs_.push_back(carry); }
    }

    auto trim() -> void {
        while (!limbs_.empty() && limbs_.back() == 0) { limbs_.pop_back(); }
    }

    std::vector<u32> limbs_;
};

constexpr u32 QUAD_FRACTION_BITS{112};
constexpr i64 QUAD_BIAS{16'383};
constexpr i64 QUAD_MIN_EXPONENT{-16'382};
// Exponent of a quad subnormal's lowest significand bit
constexpr i64 QUAD_SUBNORMAL_EXPONENT{QUAD_MIN_EXPONENT - QUAD_FRACTION_BITS};
const u128    QUAD_SIGN_BIT{u128{1} << 127};
const u128    QUAD_FRACTION_MASK{(u128{1} << QUAD_FRACTION_BITS) - 1};
constexpr u64 QUAD_EXPONENT_MASK{0x7fff};

[[nodiscard]] auto significant_bits(u128 value) noexcept -> u32 {
    return value.high != 0 ? 64 + static_cast<u32>(std::bit_width(value.high))
                           : static_cast<u32>(std::bit_width(value.low));
}

[[nodiscard]] auto low_mask(u32 bits) -> u128 {
    return bits >= 128 ? ~u128{0} : (u128{1} << bits) - 1;
}

// A finite value as `significand * 2^exponent`
struct unpacked {
    bool negative{false};
    u128 significand{};
    i64  exponent{0};
};

[[nodiscard]] auto biased_exponent(u128 bits) noexcept -> u64 {
    return static_cast<u64>(bits >> QUAD_FRACTION_BITS) & QUAD_EXPONENT_MASK;
}

[[nodiscard]] auto unpack(u128 bits) -> unpacked {
    const auto biased{static_cast<i64>(biased_exponent(bits))};
    const auto fraction{bits & QUAD_FRACTION_MASK};
    const bool negative{(bits & QUAD_SIGN_BIT) != 0};
    if (biased == 0) { return {negative, fraction, QUAD_SUBNORMAL_EXPONENT}; }
    return {negative,
            fraction | (u128{1} << QUAD_FRACTION_BITS),
            biased - QUAD_BIAS - QUAD_FRACTION_BITS};
}

// The quad holding exactly `significand * 2^exponent`, which the caller guarantees is representable
[[nodiscard]] auto exact_quad(bool negative, u128 significand, i64 exponent) -> float128 {
    const u128 sign{negative ? QUAD_SIGN_BIT : u128{}};
    if (significand == 0) { return float128::from_bits(sign); }
    const auto width{significant_bits(significand)};
    const auto top_exponent{exponent + width - 1};
    if (top_exponent >= QUAD_MIN_EXPONENT) {
        const auto biased{static_cast<u64>(top_exponent + QUAD_BIAS)};
        const auto fraction{(significand << (QUAD_FRACTION_BITS + 1 - width)) & QUAD_FRACTION_MASK};
        return float128::from_bits(sign | (u128{biased} << QUAD_FRACTION_BITS) | fraction);
    }
    return float128::from_bits(
        sign | (significand << static_cast<u32>(exponent - QUAD_SUBNORMAL_EXPONENT)));
}

// Rounds `significand * 2^exponent` (plus a nonzero tail below it when `sticky`) into `format`
[[nodiscard]] auto
round_exact(bool negative, big_uint significand, i64 exponent, bool sticky, float_format format)
    -> float128 {
    if (significand.is_zero()) { return float128::zero(negative); }
    const auto info{format_info(format)};
    const auto length{static_cast<i64>(significand.bit_length())};
    const auto top_exponent{exponent + length - 1};
    if (top_exponent > info.max_exponent) { return float128::infinity(negative); }

    // Below the normal range the format keeps fewer significand bits
    i64 kept_bits{info.precision};
    if (top_exponent < info.min_exponent) { kept_bits -= info.min_exponent - top_exponent; }
    const i64 dropped_bits{length - kept_bits};

    u128 rounded{};
    bool guard{false};
    if (dropped_bits <= 0) {
        rounded = significand.low_u128() << static_cast<u32>(-dropped_bits);
    } else {
        const auto guard_index{static_cast<u64>(dropped_bits - 1)};
        guard  = significand.bit(guard_index);
        sticky = sticky || significand.any_below(std::min(guard_index, static_cast<u64>(length)));
        significand.shift_right(static_cast<u64>(dropped_bits));
        rounded = significand.low_u128();
    }
    exponent += dropped_bits;
    if (guard && (sticky || (rounded & 1) != 0)) { ++rounded; }

    if (significant_bits(rounded) > info.precision) {
        rounded >>= 1;
        ++exponent;
    }
    if (rounded != 0 && exponent + significant_bits(rounded) - 1 > info.max_exponent) {
        return float128::infinity(negative);
    }
    return exact_quad(negative, rounded, exponent);
}

[[nodiscard]] auto round_exact(unpacked value, float_format format) -> float128 {
    return round_exact(value.negative, big_uint{value.significand}, value.exponent, false, format);
}

// Rounds the exact sum of two signed `significand * 2^exponent` terms
[[nodiscard]] auto add_exact(bool         lhs_negative,
                             big_uint     lhs,
                             i64          lhs_exponent,
                             bool         rhs_negative,
                             big_uint     rhs,
                             i64          rhs_exponent,
                             float_format format) -> float128 {
    const auto common_exponent{std::min(lhs_exponent, rhs_exponent)};
    lhs.shift_left(static_cast<u64>(lhs_exponent - common_exponent));
    rhs.shift_left(static_cast<u64>(rhs_exponent - common_exponent));
    if (lhs_negative == rhs_negative) {
        lhs.add(rhs);
        return round_exact(lhs_negative, std::move(lhs), common_exponent, false, format);
    }
    const auto order{lhs <=> rhs};
    if (order == std::strong_ordering::equal) { return float128::zero(); }
    if (order == std::strong_ordering::greater) {
        lhs.subtract(rhs);
        return round_exact(lhs_negative, std::move(lhs), common_exponent, false, format);
    }
    rhs.subtract(lhs);
    return round_exact(rhs_negative, std::move(rhs), common_exponent, false, format);
}

[[nodiscard]] auto is_hex_digit(char c) noexcept -> bool {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

[[nodiscard]] auto hex_digit_value(char c) noexcept -> u32 {
    if (c >= '0' && c <= '9') { return static_cast<u32>(c - '0'); }
    if (c >= 'a' && c <= 'f') { return static_cast<u32>(c - 'a' + 10); }
    return static_cast<u32>(c - 'A' + 10);
}

[[nodiscard]] auto is_decimal_digit(char c) noexcept -> bool { return c >= '0' && c <= '9'; }

// A signed decimal exponent, saturated well past any representable float
[[nodiscard]] auto parse_exponent(std::string_view text) -> stdx::option<i64> {
    constexpr i64 SATURATION{1'000'000'000'000};
    bool          negative{false};
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty()) { return stdx::none; }
    i64 value{0};
    for (const char c : text) {
        if (!is_decimal_digit(c)) { return stdx::none; }
        value = std::min(SATURATION, (value * 10) + (c - '0'));
    }
    return negative ? -value : value;
}

struct scanned_number {
    std::string_view whole_digits;
    std::string_view fraction_digits;
    i64              exponent{0};
};

// Splits `<digits>[.<digits>][<marker><exponent>]`, rejecting anything else
[[nodiscard]] auto scan_number(std::string_view text, bool hex) -> stdx::option<scanned_number> {
    const auto is_digit{[hex](char c) { return hex ? is_hex_digit(c) : is_decimal_digit(c); }};
    const auto take_digits{[&](std::string_view& rest) {
        usize count{0};
        while (count < rest.size() && is_digit(rest[count])) { ++count; }
        const auto digits{rest.substr(0, count)};
        rest.remove_prefix(count);
        return digits;
    }};

    scanned_number number;
    number.whole_digits = take_digits(text);
    if (!text.empty() && text.front() == '.') {
        text.remove_prefix(1);
        number.fraction_digits = take_digits(text);
    }
    if (number.whole_digits.empty() && number.fraction_digits.empty()) { return stdx::none; }
    if (text.empty()) { return number; }

    const bool is_marker{hex ? (text.front() == 'p' || text.front() == 'P')
                             : (text.front() == 'e' || text.front() == 'E')};
    if (!is_marker) { return stdx::none; }
    const auto exponent{parse_exponent(text.substr(1))};
    if (!exponent) { return stdx::none; }
    number.exponent = *exponent;
    return number;
}

[[nodiscard]] auto status_of(float128 value) noexcept -> float_parse_status {
    if (value.is_infinite()) { return float_parse_status::TOO_LARGE; }
    if (value.is_zero()) { return float_parse_status::TOO_SMALL; }
    return float_parse_status::OK;
}

[[nodiscard]] auto parse_hex(std::string_view text, bool negative, float_format format)
    -> float_parse_result {
    const auto number{scan_number(text, true)};
    if (!number) { return {float128::zero(), float_parse_status::MALFORMED}; }

    big_uint significand;
    for (const auto digits : {number->whole_digits, number->fraction_digits}) {
        for (const char c : digits) { significand.multiply_add_small(16, hex_digit_value(c)); }
    }
    if (significand.is_zero()) { return {float128::zero(negative), float_parse_status::OK}; }
    const auto exponent{number->exponent - (4 * static_cast<i64>(number->fraction_digits.size()))};
    const auto value{round_exact(negative, std::move(significand), exponent, false, format)};
    return {value, status_of(value)};
}

[[nodiscard]] auto parse_decimal(std::string_view text, bool negative, float_format format)
    -> float_parse_result {
    const auto number{scan_number(text, false)};
    if (!number) { return {float128::zero(), float_parse_status::MALFORMED}; }

    big_uint significand;
    i64      significant_digits{0};
    for (const auto digits : {number->whole_digits, number->fraction_digits}) {
        for (const char c : digits) {
            significand.multiply_add_small(10, static_cast<u32>(c - '0'));
            if (!significand.is_zero()) { ++significant_digits; }
        }
    }
    if (significand.is_zero()) { return {float128::zero(negative), float_parse_status::OK}; }

    // Outside these bounds every format overflows or rounds to zero, so skip the exact math
    const auto exponent{number->exponent - static_cast<i64>(number->fraction_digits.size())};
    const auto scientific_exponent{significant_digits - 1 + exponent};
    if (scientific_exponent > 4'933) {
        return {float128::infinity(negative), float_parse_status::TOO_LARGE};
    }
    if (scientific_exponent < -4'967) {
        return {float128::zero(negative), float_parse_status::TOO_SMALL};
    }

    if (exponent >= 0) {
        auto       scaled{significand * big_uint::power(10, static_cast<u64>(exponent))};
        const auto value{round_exact(negative, std::move(scaled), 0, false, format)};
        return {value, status_of(value)};
    }

    // Enough quotient bits that rounding sees a guard bit, with the remainder as sticky
    const auto divisor{big_uint::power(10, static_cast<u64>(-exponent))};
    const auto shift{std::max<i64>(0,
                                   static_cast<i64>(divisor.bit_length()) -
                                       static_cast<i64>(significand.bit_length()) +
                                       format_info(format).precision + 2)};
    significand.shift_left(static_cast<u64>(shift));
    const auto quotient{big_uint::divide(significand, divisor)};
    const auto value{round_exact(negative, quotient, -shift, !significand.is_zero(), format)};
    return {value, status_of(value)};
}

// `value = significand * 2^exponent` with the significand widened to the format's precision
struct format_significand {
    u128 significand;
    i64  exponent;
    bool lower_gap_halved; // the next value down is half as far away as the next one up
};

[[nodiscard]] auto normalize_to(unpacked value, float_format format) -> format_significand {
    const auto info{format_info(format)};
    const auto subnormal_exponent{static_cast<i64>(info.min_exponent) - info.precision + 1};
    const auto width{static_cast<i64>(significant_bits(value.significand))};
    auto       exponent{std::max(value.exponent + width - info.precision, subnormal_exponent)};
    // Exact both ways: the value already fits the format
    const auto shift{value.exponent - exponent};
    const auto significand{shift >= 0 ? value.significand << static_cast<u32>(shift)
                                      : value.significand >> static_cast<u32>(-shift)};
    const bool lower_gap_halved{significand == (u128{1} << (info.precision - 1)) &&
                                exponent > subnormal_exponent};
    return {significand, exponent, lower_gap_halved};
}

// `digits * 10^exponent`
struct decimal {
    std::string digits;
    i64         exponent;
};

[[nodiscard]] auto trim_trailing_zeros(decimal number) -> decimal {
    const auto last{number.digits.find_last_not_of('0')};
    number.exponent += static_cast<i64>(number.digits.size() - last - 1);
    number.digits.resize(last + 1);
    return number;
}

// The digit string incremented by one in its last place
[[nodiscard]] auto increment_digits(std::string digits) -> std::string {
    for (usize i{digits.size()}; i-- > 0;) {
        if (digits[i] != '9') {
            ++digits[i];
            return digits;
        }
        digits[i] = '0';
    }
    return "1" + digits;
}

// fmt's shortest-float layout: fixed notation for exponents in [-4, 16), scientific otherwise
[[nodiscard]] auto layout_decimal(bool negative, const decimal& number) -> std::string {
    std::string out{negative ? "-" : ""};
    const auto& digits{number.digits};
    const auto  count{static_cast<i64>(digits.size())};
    const auto  scientific_exponent{number.exponent + count - 1};

    if (scientific_exponent >= -4 && scientific_exponent < 16) {
        if (number.exponent >= 0) {
            out += digits;
            out.append(static_cast<usize>(number.exponent), '0');
        } else if (scientific_exponent >= 0) {
            const auto point{static_cast<usize>(scientific_exponent + 1)};
            out += digits.substr(0, point);
            out += '.';
            out += digits.substr(point);
        } else {
            out += "0.";
            out.append(static_cast<usize>(-scientific_exponent - 1), '0');
            out += digits;
        }
        return out;
    }

    out += digits.front();
    if (count > 1) {
        out += '.';
        out += digits.substr(1);
    }
    const auto magnitude{scientific_exponent < 0 ? -scientific_exponent : scientific_exponent};
    out += scientific_exponent < 0 ? "e-" : "e+";
    if (magnitude < 10) { out += '0'; }
    out += std::to_string(magnitude);
    return out;
}

// Compares two decimal digit strings without leading zeros
[[nodiscard]] auto compare_digits(std::string_view lhs, std::string_view rhs)
    -> std::strong_ordering {
    if (lhs.size() != rhs.size()) { return lhs.size() <=> rhs.size(); }
    return lhs.compare(rhs) <=> 0;
}

// Candidate decimals are checked against the exact midpoints to the neighboring values
class round_trip_interval {
  public:
    explicit round_trip_interval(const format_significand& value)
        : significand_is_even_{(value.significand & 1) == 0} {
        const auto scaled{value.significand << 2};
        const auto lower_gap{value.lower_gap_halved ? u128{1} : u128{2}};
        std::array bounds{big_uint{scaled - lower_gap}, big_uint{scaled}, big_uint{scaled + 2}};

        // Midpoints are `n * 2^(exponent - 2)`; express them as integers times `10^scale_`
        const auto binary_exponent{value.exponent - 2};
        if (binary_exponent >= 0) {
            for (auto& bound : bounds) { bound.shift_left(static_cast<u64>(binary_exponent)); }
        } else {
            const auto fives{big_uint::power(5, static_cast<u64>(-binary_exponent))};
            for (auto& bound : bounds) { bound = bound * fives; }
            scale_ = binary_exponent;
        }
        low_   = bounds[0].to_decimal();
        exact_ = bounds[1].to_decimal();
        high_  = bounds[2].to_decimal();
    }

    [[nodiscard]] auto exact_decimal() const -> decimal {
        return trim_trailing_zeros({exact_, scale_});
    }

    [[nodiscard]] auto contains(const decimal& candidate) const -> bool {
        auto scaled{candidate.digits};
        scaled.append(static_cast<usize>(candidate.exponent - scale_), '0');
        const auto above_low{compare_digits(scaled, low_)};
        const auto below_high{compare_digits(scaled, high_)};
        if (above_low == std::strong_ordering::less ||
            below_high == std::strong_ordering::greater) {
            return false;
        }
        // Landing exactly on a midpoint rounds to the neighbor with the even significand
        const bool on_midpoint{above_low == std::strong_ordering::equal ||
                               below_high == std::strong_ordering::equal};
        return !on_midpoint || significand_is_even_;
    }

  private:
    std::string low_;
    std::string exact_;
    std::string high_;
    i64         scale_{0};
    bool        significand_is_even_;
};

// The nearer of the two `length`-digit neighbors first, preferring an even last digit on a tie
[[nodiscard]] auto candidates_of(const decimal& exact, usize length) -> std::array<decimal, 2> {
    const auto dropped{exact.digits.substr(length)};
    decimal    floor{exact.digits.substr(0, length),
                  exact.exponent + static_cast<i64>(exact.digits.size() - length)};
    decimal    ceil{increment_digits(floor.digits), floor.exponent};

    const auto first_dropped{dropped.front()};
    const bool rest_nonzero{dropped.find_first_not_of('0', 1) != std::string_view::npos};
    bool       ceil_first{first_dropped > '5' || (first_dropped == '5' && rest_nonzero)};
    if (first_dropped == '5' && !rest_nonzero) {
        ceil_first = (floor.digits.back() - '0') % 2 != 0;
    }
    if (ceil_first) { return {std::move(ceil), std::move(floor)}; }
    return {std::move(floor), std::move(ceil)};
}

} // namespace

auto format_info(float_format format) noexcept -> float_format_info {
    switch (format) {
    case float_format::HALF:   return {11, -14, 15, 16};
    case float_format::SINGLE: return {24, -126, 127, 32};
    case float_format::DOUBLE: return {53, -1'022, 1'023, 64};
    case float_format::X87:    return {64, -16'382, 16'383, 80};
    case float_format::QUAD:   return {113, -16'382, 16'383, 128};
    }
    return {113, -16'382, 16'383, 128};
}

auto float128::from_bits(u128 bits) noexcept -> float128 {
    float128 value;
    value.bits_ = bits;
    return value;
}

auto float128::from_f64(f64 value) -> float128 {
    return decode(u128{std::bit_cast<u64>(value)}, float_format::DOUBLE);
}

auto float128::from_int(i128 value, float_format format) -> float128 {
    const bool negative{value < 0};
    auto       magnitude{static_cast<u128>(value)};
    if (negative) { magnitude = ~magnitude + 1; }
    return round_exact(negative, big_uint{magnitude}, 0, false, format);
}

auto float128::from_uint(u128 value, float_format format) -> float128 {
    return round_exact(false, big_uint{value}, 0, false, format);
}

auto float128::decode(u128 bits, float_format format) -> float128 {
    const auto info{format_info(format)};
    const bool explicit_integer_bit{format == float_format::X87};
    const auto field_bits{explicit_integer_bit ? info.precision : info.precision - 1};
    const auto exponent_bits{info.storage_bits - 1 - field_bits};
    const auto all_ones{(u64{1} << exponent_bits) - 1};

    const bool negative{((bits >> (info.storage_bits - 1)) & 1) != 0};
    const auto biased{static_cast<u64>(bits >> field_bits) & all_ones};
    const auto field{bits & low_mask(field_bits)};
    if (biased == all_ones) {
        const auto payload{explicit_integer_bit ? field & low_mask(field_bits - 1) : field};
        return payload == 0 ? infinity(negative) : quiet_nan();
    }

    auto significand{field};
    if (!explicit_integer_bit && biased != 0) { significand |= u128{1} << field_bits; }
    const auto exponent{static_cast<i64>(std::max<u64>(biased, 1)) - info.max_exponent -
                        (info.precision - 1)};
    return round_exact(negative, big_uint{significand}, exponent, false, float_format::QUAD);
}

auto float128::infinity(bool negative) noexcept -> float128 {
    return from_bits((negative ? QUAD_SIGN_BIT : u128{}) |
                     (u128{QUAD_EXPONENT_MASK} << QUAD_FRACTION_BITS));
}

auto float128::quiet_nan() noexcept -> float128 {
    return from_bits((u128{QUAD_EXPONENT_MASK} << QUAD_FRACTION_BITS) |
                     (u128{1} << (QUAD_FRACTION_BITS - 1)));
}

auto float128::zero(bool negative) noexcept -> float128 {
    return from_bits(negative ? QUAD_SIGN_BIT : u128{});
}

auto float128::parse(std::string_view text, float_format format) -> float_parse_result {
    bool negative{false};
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        return parse_hex(text.substr(2), negative, format);
    }
    return parse_decimal(text, negative, format);
}

auto float128::encode(float_format format) const -> u128 {
    const auto info{format_info(format)};
    const bool explicit_integer_bit{format == float_format::X87};
    const auto field_bits{explicit_integer_bit ? info.precision : info.precision - 1};
    const auto exponent_bits{info.storage_bits - 1 - field_bits};
    const auto all_ones{u128{(u64{1} << exponent_bits) - 1}};
    const auto integer_bit{explicit_integer_bit ? u128{1} << (field_bits - 1) : u128{}};

    const auto rounded{round_to(format)};
    const u128 sign{rounded.is_negative() ? u128{1} << (info.storage_bits - 1) : u128{}};
    if (rounded.is_nan()) {
        return (all_ones << field_bits) | integer_bit | (u128{1} << (info.precision - 2));
    }
    if (rounded.is_infinite()) { return sign | (all_ones << field_bits) | integer_bit; }
    if (rounded.is_zero()) { return sign; }

    const auto normalized{normalize_to(unpack(rounded.bits_), format)};
    const bool subnormal{(normalized.significand >> (info.precision - 1)) == 0};
    const auto biased{subnormal ? u128{}
                                : u128{static_cast<u64>(normalized.exponent + info.precision - 1 +
                                                        info.max_exponent)}};
    const auto field{explicit_integer_bit ? normalized.significand
                                          : normalized.significand & low_mask(field_bits)};
    return sign | (biased << field_bits) | field;
}

auto float128::round_to(float_format format) const -> float128 {
    if (format == float_format::QUAD || !is_finite() || is_zero()) { return *this; }
    return round_exact(unpack(bits_), format);
}

auto float128::to_f64() const -> f64 {
    return std::bit_cast<f64>(encode(float_format::DOUBLE).low);
}

auto float128::trunc() const -> float128 {
    if (!is_finite() || is_zero()) { return *this; }
    const auto value{unpack(bits_)};
    if (value.exponent >= 0) { return *this; }
    if (value.exponent <= -static_cast<i64>(QUAD_FRACTION_BITS + 1)) {
        return zero(value.negative);
    }
    return exact_quad(value.negative, value.significand >> static_cast<u32>(-value.exponent), 0);
}

auto float128::to_int() const -> stdx::option<i128> {
    const auto magnitude{abs().to_uint()};
    if (!magnitude) { return stdx::none; }
    const u128 min_magnitude{u128{1} << 127};
    if (is_negative()) {
        if (*magnitude > min_magnitude) { return stdx::none; }
        return static_cast<i128>(~*magnitude + 1);
    }
    if (*magnitude >= min_magnitude) { return stdx::none; }
    return static_cast<i128>(*magnitude);
}

auto float128::to_uint() const -> stdx::option<u128> {
    const auto truncated{trunc()};
    if (!truncated.is_finite()) { return stdx::none; }
    if (truncated.is_zero()) { return u128{}; }
    if (truncated.is_negative()) { return stdx::none; }
    const auto value{unpack(truncated.bits_)};
    if (value.exponent < 0) { return value.significand >> static_cast<u32>(-value.exponent); }
    if (significant_bits(value.significand) + value.exponent > 128) { return stdx::none; }
    return value.significand << static_cast<u32>(value.exponent);
}

auto float128::to_string(float_format format) const -> std::string {
    const auto rounded{round_to(format)};
    if (rounded.is_nan()) { return "nan"; }
    if (rounded.is_infinite()) { return rounded.is_negative() ? "-inf" : "inf"; }
    if (rounded.is_zero()) { return rounded.is_negative() ? "-0" : "0"; }

    const round_trip_interval interval{normalize_to(unpack(rounded.bits_), format)};
    const auto                exact{interval.exact_decimal()};
    for (usize length{1}; length < exact.digits.size(); ++length) {
        for (auto& candidate : candidates_of(exact, length)) {
            if (interval.contains(candidate)) {
                return layout_decimal(rounded.is_negative(), trim_trailing_zeros(candidate));
            }
        }
    }
    return layout_decimal(rounded.is_negative(), exact);
}

auto float128::is_nan() const noexcept -> bool {
    return biased_exponent(bits_) == QUAD_EXPONENT_MASK && (bits_ & QUAD_FRACTION_MASK) != 0;
}

auto float128::is_infinite() const noexcept -> bool {
    return biased_exponent(bits_) == QUAD_EXPONENT_MASK && (bits_ & QUAD_FRACTION_MASK) == 0;
}

auto float128::is_finite() const noexcept -> bool {
    return biased_exponent(bits_) != QUAD_EXPONENT_MASK;
}

auto float128::is_zero() const noexcept -> bool { return (bits_ & ~QUAD_SIGN_BIT) == 0; }

auto float128::is_negative() const noexcept -> bool { return (bits_ & QUAD_SIGN_BIT) != 0; }

auto float128::operator-() const noexcept -> float128 { return from_bits(bits_ ^ QUAD_SIGN_BIT); }

auto float128::abs() const noexcept -> float128 { return from_bits(bits_ & ~QUAD_SIGN_BIT); }

auto add(float128 lhs, float128 rhs, float_format format) -> float128 {
    if (lhs.is_nan() || rhs.is_nan()) { return float128::quiet_nan(); }
    if (lhs.is_infinite() || rhs.is_infinite()) {
        if (lhs.is_infinite() && rhs.is_infinite() && lhs.is_negative() != rhs.is_negative()) {
            return float128::quiet_nan();
        }
        return lhs.is_infinite() ? lhs : rhs;
    }
    if (lhs.is_zero() && rhs.is_zero()) {
        return float128::zero(lhs.is_negative() && rhs.is_negative());
    }
    const auto l{unpack(lhs.bits())};
    const auto r{unpack(rhs.bits())};
    return add_exact(l.negative,
                     big_uint{l.significand},
                     l.exponent,
                     r.negative,
                     big_uint{r.significand},
                     r.exponent,
                     format);
}

auto subtract(float128 lhs, float128 rhs, float_format format) -> float128 {
    return add(lhs, -rhs, format);
}

auto multiply(float128 lhs, float128 rhs, float_format format) -> float128 {
    const bool negative{lhs.is_negative() != rhs.is_negative()};
    if (lhs.is_nan() || rhs.is_nan()) { return float128::quiet_nan(); }
    if (lhs.is_infinite() || rhs.is_infinite()) {
        if (lhs.is_zero() || rhs.is_zero()) { return float128::quiet_nan(); }
        return float128::infinity(negative);
    }
    if (lhs.is_zero() || rhs.is_zero()) { return float128::zero(negative); }
    const auto l{unpack(lhs.bits())};
    const auto r{unpack(rhs.bits())};
    return round_exact(negative,
                       big_uint{l.significand} * big_uint{r.significand},
                       l.exponent + r.exponent,
                       false,
                       format);
}

auto divide(float128 lhs, float128 rhs, float_format format) -> float128 {
    const bool negative{lhs.is_negative() != rhs.is_negative()};
    if (lhs.is_nan() || rhs.is_nan()) { return float128::quiet_nan(); }
    if (lhs.is_infinite()) {
        return rhs.is_infinite() ? float128::quiet_nan() : float128::infinity(negative);
    }
    if (rhs.is_infinite()) { return float128::zero(negative); }
    if (rhs.is_zero()) {
        return lhs.is_zero() ? float128::quiet_nan() : float128::infinity(negative);
    }
    if (lhs.is_zero()) { return float128::zero(negative); }

    const auto     l{unpack(lhs.bits())};
    const auto     r{unpack(rhs.bits())};
    big_uint       numerator{l.significand};
    const big_uint divisor{r.significand};
    const auto     shift{std::max<i64>(0,
                                   static_cast<i64>(divisor.bit_length()) -
                                       static_cast<i64>(numerator.bit_length()) +
                                       format_info(format).precision + 2)};
    numerator.shift_left(static_cast<u64>(shift));
    auto quotient{big_uint::divide(numerator, divisor)};
    return round_exact(negative,
                       std::move(quotient),
                       l.exponent - r.exponent - shift,
                       !numerator.is_zero(),
                       format);
}

auto fused_multiply_add(float128 a, float128 b, float128 c, float_format format) -> float128 {
    if (a.is_nan() || b.is_nan() || c.is_nan()) { return float128::quiet_nan(); }
    const bool product_negative{a.is_negative() != b.is_negative()};
    if (a.is_infinite() || b.is_infinite()) {
        if (a.is_zero() || b.is_zero()) { return float128::quiet_nan(); }
        return add(float128::infinity(product_negative), c, format);
    }
    if (c.is_infinite()) { return c; }
    if (a.is_zero() || b.is_zero()) { return add(float128::zero(product_negative), c, format); }

    const auto x{unpack(a.bits())};
    const auto y{unpack(b.bits())};
    auto       product{big_uint{x.significand} * big_uint{y.significand}};
    const auto product_exponent{x.exponent + y.exponent};
    if (c.is_zero()) {
        return round_exact(product_negative, std::move(product), product_exponent, false, format);
    }
    const auto z{unpack(c.bits())};
    return add_exact(product_negative,
                     std::move(product),
                     product_exponent,
                     z.negative,
                     big_uint{z.significand},
                     z.exponent,
                     format);
}

auto float128::operator==(const float128& other) const noexcept -> bool {
    if (is_nan() || other.is_nan()) { return false; }
    if (is_zero() && other.is_zero()) { return true; }
    return bits_ == other.bits_;
}

auto float128::operator<=>(const float128& other) const noexcept -> std::partial_ordering {
    if (is_nan() || other.is_nan()) { return std::partial_ordering::unordered; }
    if (is_zero() && other.is_zero()) { return std::partial_ordering::equivalent; }
    if (is_negative() != other.is_negative()) {
        return is_negative() ? std::partial_ordering::less : std::partial_ordering::greater;
    }
    const auto magnitude{bits_ & ~QUAD_SIGN_BIT};
    const auto other_magnitude{other.bits_ & ~QUAD_SIGN_BIT};
    if (magnitude == other_magnitude) { return std::partial_ordering::equivalent; }
    const bool smaller_magnitude{magnitude < other_magnitude};
    return smaller_magnitude != is_negative() ? std::partial_ordering::less
                                              : std::partial_ordering::greater;
}

} // namespace ghoti
