#pragma once

#include <array>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/base.h>
#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>

namespace ghoti::test {

template <typename T>
concept Unwrappable =
    stdx::Option<std::remove_cvref_t<T>> || stdx::Result<std::remove_cvref_t<T>> ||
    stdx::OptSize<std::remove_cvref_t<T>> || stdx::Pointer<std::remove_cvref_t<T>>;

// Unpacks the value in the option or result and returns its value if present
template <Unwrappable U>
[[nodiscard]] auto unwrap(U&& u, std::string_view expr, std::string_view file, i32 line)
    -> decltype(auto) {
    if (!u) { fmt::println("unwrap called on disengaged value ({}): {}:{}", expr, file, line); }
    REQUIRE(u);
    return *std::forward<U>(u);
}

// Unpacks the value in the option or result and returns its value if present and equal to expected
template <Unwrappable U, typename E>
[[nodiscard]] auto
unwrap(U&& u, E&& expected_value, std::string_view expr, std::string_view file, i32 line)
    -> decltype(auto) {
    decltype(auto) unwrapped{unwrap(std::forward<U>(u), expr, file, line)};
    REQUIRE(unwrapped == std::forward<E>(expected_value));
    return unwrapped;
}

template <Unwrappable U>
auto unwrap_err(U&& u, std::string_view expr, std::string_view file, int line) -> decltype(auto) {
    if (u) { fmt::println("unwrap_err called on engaged value ({}): {}:{}", expr, file, line); }
    using T = std::remove_cvref_t<U>;
    if constexpr (stdx::Option<T>) {
        REQUIRE_FALSE(u);
    } else if constexpr (stdx::Result<T>) {
        REQUIRE_FALSE(u);
        return u.error();
    }
}

template <typename T, typename... Ts> auto make_vector(Ts&&... es) -> std::vector<T> {
    std::vector<T> list;
    list.reserve(sizeof...(es));
    (list.emplace_back(std::forward<Ts>(es)), ...);
    return list;
}

// Calculates the array's combinations with itself, given by:
// [A, B, C] -> [[A, B], [A, C], [B, B]]
template <typename T, usize N> constexpr auto combinations(std::array<T, N> input) {
    constexpr auto                    size{(N * (N - 1)) / 2};
    std::array<std::pair<T, T>, size> results;
    if (input.size() < 2) { return results; }

    for (usize i{0}, idx{0}; i < input.size() - 1; ++i) {
        for (usize j{i + 1}; j < input.size(); ++j) { results[idx++] = {input[i], input[j]}; }
    }
    return results;
}

} // namespace ghoti::test

#define UNWRAP_1(expr) ::ghoti::test::unwrap((expr), #expr, __FILE__, __LINE__)
#define UNWRAP_2(expr, expected) \
    ::ghoti::test::unwrap((expr), (expected), #expr, __FILE__, __LINE__)

#define UNWRAP_MACRO(_1, _2, NAME, ...) NAME
#define UNWRAP(...) UNWRAP_MACRO(__VA_ARGS__, UNWRAP_2, UNWRAP_1)(__VA_ARGS__)

#define UNWRAP_ERR(expr) ::ghoti::test::unwrap_err((expr), #expr, __FILE__, __LINE__)
