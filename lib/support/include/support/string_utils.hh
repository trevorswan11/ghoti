#pragma once

#include <array>
#include <string_view>

#include <stdx/assert.hh>
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

} // namespace ghoti::string_utils
