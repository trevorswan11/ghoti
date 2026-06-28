#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <type_traits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/module/module.hh"

namespace ghoti::tests::helpers {

template <typename T>
concept Unwrappable = stdx::Option<std::remove_cvref_t<T>> ||
                      stdx::Result<std::remove_cvref_t<T>> || stdx::OptSize<std::remove_cvref_t<T>>;

// Unpacks the value in the option or result and returns its value if present
template <Unwrappable U> [[nodiscard]] auto unwrap(U&& u) -> decltype(auto) {
    REQUIRE(u);
    return *u;
}

// Unpacks the value in the option or result and returns its value if present and equal to expected
template <Unwrappable U, typename E>
[[nodiscard]] auto unwrap(U&& u, E&& expected_value) -> decltype(auto) {
    REQUIRE(u);
    REQUIRE(*u == std::forward<E>(expected_value));
    return *u;
}

template <Unwrappable U> auto unwrap_err(U&& u) -> decltype(auto) {
    using T = std::remove_cvref_t<U>;
    if constexpr (stdx::Option<T>) {
        REQUIRE_FALSE(u);
    } else if constexpr (stdx::Result<T>) {
        REQUIRE_FALSE(u);
        return u.error();
    }
}

// Checks if the error list is empty, dumping the list's contents otherwise.
template <typename E> auto check_errors(gsl::span<const E> errors) {
    if (!errors.empty()) { fmt::println("{}", errors); }
    REQUIRE(errors.empty());
}

// Checks if the error list is empty, dumping the list's contents otherwise.
template <typename DiagList> auto check_errors(const mod::module& module) {
    if (const auto diags{module.diagnostics.as_opt<DiagList>()}) {
        check_errors<typename DiagList::value_type>(*diags);
    }
}

// Checks if the error list is matches the expected, dumping the list's contents otherwise.
template <typename E, std::same_as<E>... Es>
auto check_errors_against(gsl::span<const E> errors, Es&&... expected_errors) {
    const std::array expected_arr{std::forward<Es>(expected_errors)...};
    constexpr auto   expected_count{sizeof...(Es)};

    if constexpr (expected_count == 0) {
        check_errors<E>(errors);
    } else {
        if (errors.size() != expected_count) {
            for (const auto& e : errors) { fmt::println("{}", e); }
            CHECK(errors.size() == expected_count);
        }

        const auto ranges_eq{std::ranges::equal(errors, expected_arr)};
        if (!ranges_eq) { fmt::println("{}", errors); }
        CHECK(ranges_eq);
    }
}

template <typename DiagList, std::same_as<typename DiagList::value_type>... Es>
auto check_errors_against(const mod::module& module, Es&&... expected_errors) {
    const auto& diags{unwrap(module.diagnostics.as_opt<DiagList>())};
    check_errors_against<typename DiagList::value_type>(diags,
                                                        std::forward<Es>(expected_errors)...);
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

} // namespace ghoti::tests::helpers
