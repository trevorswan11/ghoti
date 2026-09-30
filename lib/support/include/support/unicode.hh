#pragma once

#include <string_view>

#include <stdx/types.hh>

namespace ghoti {

// Terminal columns a code point takes: 0 for combining marks, joiners, and variation selectors,
// 2 for wide characters and emoji, 1 otherwise
[[nodiscard]] auto code_point_width(u32 code_point) noexcept -> usize;

// Terminal columns `text` takes. A zero-width-joiner sequence (a family emoji) and a regional
// indicator pair (a flag) are one wide cluster; malformed UTF-8 counts a column per byte.
[[nodiscard]] auto display_width(std::string_view text) noexcept -> usize;

// UTF-16 code units in `text`, counting malformed bytes as one unit each
[[nodiscard]] auto utf16_length(std::string_view text) noexcept -> usize;

// The byte offset into `text` that is `units` UTF-16 code units in, clamped to its end and never
// splitting a code point
[[nodiscard]] auto byte_offset_of_utf16(std::string_view text, usize units) noexcept -> usize;

} // namespace ghoti
