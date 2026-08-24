#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <string_view>

#include <stdx/assert.hh>
#include <stdx/fixed/hash_table.hh>
#include <stdx/hash.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>

namespace ghoti::string_utils {

template <usize MaxLen = 32> struct lowercase_str {
    std::array<char, MaxLen> data{};
    usize                    len{0};

    constexpr lowercase_str() noexcept = default;
    constexpr explicit lowercase_str(std::string_view sv) noexcept : len{sv.size()} {
        ASSERT(sv.size() < MaxLen, "Enum name exceeds buffer size");
        for (usize i{0}; i < sv.size(); ++i) { data[i] = stdx::string::to_lower(sv[i]); }
    }

    [[nodiscard]] constexpr auto view() const noexcept -> std::string_view {
        return std::string_view{data.data(), len};
    }
    [[nodiscard]] constexpr operator std::string_view() const noexcept { return view(); }
};

template <typename Value, usize Size>
using constexpr_map_t =
    stdx::fixed::hash_map<std::string_view, Value, Size, stdx::crc::hash, std::equal_to<>>;

template <typename Value, typename... Entries>
[[nodiscard]] constexpr auto make_constexpr_map(Entries&&... entries) {
    constexpr_map_t<Value, sizeof...(Entries)> map;
    using std::get;
    (map.emplace(get<0>(entries), get<1>(entries)), ...);
    return map;
}

auto to_u8string(std::string_view s) -> std::u8string;
auto to_utf8(const std::u8string& s) -> std::string;

constexpr auto strip_trailing_cr(std::string& line) -> void {
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
}

[[nodiscard]] constexpr auto contains_ci(std::string_view haystack, std::string_view needle)
    -> bool {
    return std::ranges::search(haystack, needle, [](char a, char b) {
               return stdx::string::to_lower(a) == stdx::string::to_lower(b);
           }).begin() != haystack.end();
}

[[nodiscard]] constexpr auto starts_with_ci(std::string_view line, std::string_view prefix)
    -> bool {
    return std::ranges::starts_with(
        line, prefix, {}, stdx::string::to_lower, stdx::string::to_lower);
}

} // namespace ghoti::string_utils
