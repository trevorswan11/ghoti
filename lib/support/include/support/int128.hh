#pragma once

#include <concepts>
#include <type_traits>

#include <boost/int128.hpp>
#include <fmt/base.h>
#include <fmt/format.h>

namespace ghoti {

using i128 = boost::int128::int128;
using u128 = boost::int128::uint128;

template <typename I>
concept Integral = std::integral<I> || std::same_as<I, i128> || std::same_as<I, u128>;

template <typename I>
concept Signed = std::is_signed_v<I> || std::same_as<I, i128>;

template <typename I>
concept Unsigned = std::is_unsigned_v<I> || std::same_as<I, u128>;

} // namespace ghoti

template <> struct fmt::formatter<ghoti::i128> {
    static constexpr auto parse(format_parse_context& ctx) noexcept { return ctx.begin(); }

    static auto format(ghoti::i128 i, format_context& ctx) {
        return fmt::format_to(ctx.out(), "{}", boost::int128::to_string(i));
    }
};

template <> struct fmt::formatter<ghoti::u128> {
    static constexpr auto parse(format_parse_context& ctx) noexcept { return ctx.begin(); }

    static auto format(ghoti::u128 i, format_context& ctx) {
        return fmt::format_to(ctx.out(), "{}", boost::int128::to_string(i));
    }
};
