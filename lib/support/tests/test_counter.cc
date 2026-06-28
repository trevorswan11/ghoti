#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "support/counter.hh"

namespace ghoti::tests {

TEST_CASE("Default counter") {
    default_counter c;

    CHECK(c == 0);
    SECTION("Single guard") {
        const default_counter::guard g{c};
        CHECK(c == 1);
    }
    CHECK(c == 0);

    SECTION("Nested guards") {
        const default_counter::guard g1{c};
        {
            const default_counter::guard g2{c};
            CHECK(c == 2);
        }
        CHECK(c == 1);
    }
    CHECK(c == 0);
}

TEST_CASE("Counter operators") {
    counter<i32> c;
    CHECK(c == 0);
    CHECK(c <= 0);
    CHECK(c <= 10);
    CHECK(c >= 0);
    CHECK(c >= -10);

    CHECK_FALSE(c);
    const counter<i32>::guard g{c};
    CHECK(c);
}

} // namespace ghoti::tests
