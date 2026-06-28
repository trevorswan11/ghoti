#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "support/diagnostic.hh"

namespace ghoti {

struct something_location {};
struct something_else_location {};

template <> struct source_info<something_location> {
    static auto get(const something_location&) noexcept -> source_location { return {0, 42}; }
};

template <> struct source_info<something_else_location> {
    static auto get(const something_else_location&) noexcept -> source_location { return {42, 0}; }
};

namespace tests {

enum class test_enum : u8 {
    SAD,
    MAD,
};

TEST_CASE("Diagnostic traits") {
    STATIC_CHECK(Diagnostic<diagnostic<test_enum>>);
    STATIC_CHECK_FALSE(Diagnostic<test_enum>);
}

TEST_CASE("Location and error only") {
    something_location    l;
    diagnostic<test_enum> d{test_enum::SAD, l};
    CHECK("error: SAD 1:43" == d.to_string(stdx::none, false));
}

TEST_CASE("Custom locateable") {
    something_location    l;
    diagnostic<test_enum> d{"message", test_enum::SAD, l};
    CHECK("error: message 1:43" == d.to_string(stdx::none, false));
}

TEST_CASE("Error messages with associated files") {
    diagnostic<test_enum> d{"message", test_enum::SAD};
    CHECK("foo.gh: error: message" == d.to_string("foo.gh", false));
}

TEST_CASE("Locateable Error messages with associated files") {
    something_location    l;
    diagnostic<test_enum> d{"message", test_enum::SAD, l};
    CHECK("foo.gh:1:43: error: message" == d.to_string("foo.gh", false));
}

TEST_CASE("Move constructor with new error") {
    something_location    l;
    diagnostic<test_enum> d1{"message", test_enum::SAD, l};
    diagnostic<test_enum> d2{std::move(d1), test_enum::MAD};
    CHECK("error: message 1:43" == d2.to_string(stdx::none, false));
}

TEST_CASE("Move constructor with new location") {
    something_location    l;
    diagnostic<test_enum> d1{"message", test_enum::SAD, l};

    something_else_location e;
    diagnostic<test_enum>   d2{std::move(d1), e};
    CHECK("error: message 43:1" == d2.to_string(stdx::none, false));
}

} // namespace tests

} // namespace ghoti
