#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <fmt/base.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/span>
#include <stdx/types.hh>

#include "compiler/module/module.hh"
#include "support/test.hh" // IWYU pragma: export

namespace ghoti::tests::helpers {

using namespace ::ghoti::test;

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
    const auto& diags{UNWRAP(module.diagnostics.as_opt<DiagList>())};
    check_errors_against<typename DiagList::value_type>(diags,
                                                        std::forward<Es>(expected_errors)...);
}

} // namespace ghoti::tests::helpers
