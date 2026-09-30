#include "support/unicode.hh"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

#include <stdx/types.hh>

namespace ghoti {

namespace {

struct code_point_range {
    u32 first;
    u32 last;
};

// Marks that draw over the previous character, joiners, format controls, and selectors
constexpr std::array ZERO_WIDTH{
    code_point_range{0x0300, 0x036F},   code_point_range{0x0483, 0x0489},
    code_point_range{0x0591, 0x05BD},   code_point_range{0x05BF, 0x05BF},
    code_point_range{0x05C1, 0x05C2},   code_point_range{0x05C4, 0x05C5},
    code_point_range{0x05C7, 0x05C7},   code_point_range{0x0610, 0x061A},
    code_point_range{0x064B, 0x065F},   code_point_range{0x0670, 0x0670},
    code_point_range{0x06D6, 0x06DC},   code_point_range{0x06DF, 0x06E4},
    code_point_range{0x06E7, 0x06E8},   code_point_range{0x06EA, 0x06ED},
    code_point_range{0x0711, 0x0711},   code_point_range{0x0730, 0x074A},
    code_point_range{0x0900, 0x0902},   code_point_range{0x093A, 0x093A},
    code_point_range{0x093C, 0x093C},   code_point_range{0x0941, 0x0948},
    code_point_range{0x094D, 0x094D},   code_point_range{0x0951, 0x0957},
    code_point_range{0x0962, 0x0963},   code_point_range{0x0E31, 0x0E31},
    code_point_range{0x0E34, 0x0E3A},   code_point_range{0x0E47, 0x0E4E},
    code_point_range{0x1AB0, 0x1AFF},   code_point_range{0x1DC0, 0x1DFF},
    code_point_range{0x200B, 0x200F},   code_point_range{0x202A, 0x202E},
    code_point_range{0x2060, 0x2064},   code_point_range{0x20D0, 0x20FF},
    code_point_range{0xFE00, 0xFE0F},   code_point_range{0xFE20, 0xFE2F},
    code_point_range{0xFEFF, 0xFEFF},   code_point_range{0x1F3FB, 0x1F3FF},
    code_point_range{0xE0000, 0xE007F}, code_point_range{0xE0100, 0xE01EF},
};

// East Asian Wide and Fullwidth characters, and emoji that present as wide
constexpr std::array WIDE{
    code_point_range{0x1100, 0x115F},   code_point_range{0x231A, 0x231B},
    code_point_range{0x2329, 0x232A},   code_point_range{0x23E9, 0x23EC},
    code_point_range{0x23F0, 0x23F0},   code_point_range{0x23F3, 0x23F3},
    code_point_range{0x25FD, 0x25FE},   code_point_range{0x2614, 0x2615},
    code_point_range{0x2648, 0x2653},   code_point_range{0x267F, 0x267F},
    code_point_range{0x2693, 0x2693},   code_point_range{0x26A1, 0x26A1},
    code_point_range{0x26AA, 0x26AB},   code_point_range{0x26BD, 0x26BE},
    code_point_range{0x26C4, 0x26C5},   code_point_range{0x26CE, 0x26CE},
    code_point_range{0x26D4, 0x26D4},   code_point_range{0x26EA, 0x26EA},
    code_point_range{0x26F2, 0x26F3},   code_point_range{0x26F5, 0x26F5},
    code_point_range{0x26FA, 0x26FA},   code_point_range{0x26FD, 0x26FD},
    code_point_range{0x2705, 0x2705},   code_point_range{0x270A, 0x270B},
    code_point_range{0x2728, 0x2728},   code_point_range{0x274C, 0x274C},
    code_point_range{0x274E, 0x274E},   code_point_range{0x2753, 0x2755},
    code_point_range{0x2757, 0x2757},   code_point_range{0x2795, 0x2797},
    code_point_range{0x27B0, 0x27B0},   code_point_range{0x27BF, 0x27BF},
    code_point_range{0x2B1B, 0x2B1C},   code_point_range{0x2B50, 0x2B50},
    code_point_range{0x2B55, 0x2B55},   code_point_range{0x2E80, 0x303E},
    code_point_range{0x3041, 0x33FF},   code_point_range{0x3400, 0x4DBF},
    code_point_range{0x4E00, 0x9FFF},   code_point_range{0xA000, 0xA4CF},
    code_point_range{0xA960, 0xA97F},   code_point_range{0xAC00, 0xD7A3},
    code_point_range{0xF900, 0xFAFF},   code_point_range{0xFE10, 0xFE19},
    code_point_range{0xFE30, 0xFE6F},   code_point_range{0xFF00, 0xFF60},
    code_point_range{0xFFE0, 0xFFE6},   code_point_range{0x16FE0, 0x16FE4},
    code_point_range{0x17000, 0x18AFF}, code_point_range{0x1B000, 0x1B2FF},
    code_point_range{0x1F004, 0x1F004}, code_point_range{0x1F0CF, 0x1F0CF},
    code_point_range{0x1F18E, 0x1F18E}, code_point_range{0x1F191, 0x1F19A},
    code_point_range{0x1F1E6, 0x1F1FF}, code_point_range{0x1F200, 0x1F202},
    code_point_range{0x1F210, 0x1F23B}, code_point_range{0x1F240, 0x1F248},
    code_point_range{0x1F250, 0x1F251}, code_point_range{0x1F260, 0x1F265},
    code_point_range{0x1F300, 0x1F64F}, code_point_range{0x1F680, 0x1F6FF},
    code_point_range{0x1F7E0, 0x1F7EB}, code_point_range{0x1F90C, 0x1F9FF},
    code_point_range{0x1FA70, 0x1FAFF}, code_point_range{0x20000, 0x2FFFD},
    code_point_range{0x30000, 0x3FFFD},
};

[[nodiscard]] constexpr auto in_ranges(const auto& ranges, u32 code_point) noexcept -> bool {
    const auto it{std::ranges::lower_bound(
        ranges, code_point, {}, [](const code_point_range& r) { return r.last; })};
    return it != ranges.end() && it->first <= code_point;
}

constexpr u32 ZERO_WIDTH_JOINER{0x200D};

[[nodiscard]] constexpr auto is_regional_indicator(u32 code_point) noexcept -> bool {
    return code_point >= 0x1F1E6 && code_point <= 0x1F1FF;
}

struct next_code_point {
    u32   value;
    usize length;
    bool  valid;
};

// Reads one code point; a malformed sequence is one byte of width one
[[nodiscard]] auto read_code_point(std::string_view text) noexcept -> next_code_point {
    const auto lead{static_cast<u8>(text.front())};
    usize      length{1};
    u32        value{lead};
    if (lead >= 0xF0 && lead < 0xF8) {
        length = 4;
        value  = lead & 0x07U;
    } else if (lead >= 0xE0 && lead < 0xF0) {
        length = 3;
        value  = lead & 0x0FU;
    } else if (lead >= 0xC0 && lead < 0xE0) {
        length = 2;
        value  = lead & 0x1FU;
    } else if (lead >= 0x80) {
        return {lead, 1, false};
    }
    if (length > text.size()) { return {lead, 1, false}; }
    for (usize k{1}; k < length; ++k) {
        const auto continuation{static_cast<u8>(text[k])};
        if ((continuation & 0xC0U) != 0x80U) { return {lead, 1, false}; }
        value = (value << 6U) | (continuation & 0x3FU);
    }
    return {value, length, true};
}

} // namespace

auto code_point_width(u32 code_point) noexcept -> usize {
    if (code_point == 0) { return 0; }
    if (code_point < 0x20 || (code_point >= 0x7F && code_point < 0xA0)) { return 0; }
    if (code_point == ZERO_WIDTH_JOINER || in_ranges(ZERO_WIDTH, code_point)) { return 0; }
    return in_ranges(WIDE, code_point) ? 2 : 1;
}

auto display_width(std::string_view text) noexcept -> usize {
    usize width{0};
    bool  joined{false};
    bool  pending_flag{false};
    for (usize i{0}; i < text.size();) {
        const auto next{read_code_point(text.substr(i))};
        i += next.length;
        if (!next.valid) {
            ++width;
            joined       = false;
            pending_flag = false;
            continue;
        }
        if (next.value == ZERO_WIDTH_JOINER) {
            joined = true;
            continue;
        }
        // The character after a joiner draws inside the cluster it joins
        if (std::exchange(joined, false)) { continue; }
        if (is_regional_indicator(next.value)) {
            if (std::exchange(pending_flag, !pending_flag)) { continue; }
        } else {
            pending_flag = false;
        }
        width += code_point_width(next.value);
    }
    return width;
}

auto utf16_length(std::string_view text) noexcept -> usize {
    usize units{0};
    for (usize i{0}; i < text.size();) {
        const auto next{read_code_point(text.substr(i))};
        i += next.length;
        units += next.valid && next.value >= 0x10000 ? 2 : 1;
    }
    return units;
}

auto byte_offset_of_utf16(std::string_view text, usize units) noexcept -> usize {
    usize counted{0};
    usize i{0};
    while (i < text.size() && counted < units) {
        const auto next{read_code_point(text.substr(i))};
        counted += next.valid && next.value >= 0x10000 ? 2 : 1;
        i += next.length;
    }
    return i;
}

} // namespace ghoti
