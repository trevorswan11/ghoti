#include <catch2/catch_test_macros.hpp>

#include "support/env.hh"

namespace ghoti::tests {

TEST_CASE("Environment variables get and set") {
    set_env("GHOTI_TEST_VAR", "hello_world");
    const auto val{get_env("GHOTI_TEST_VAR")};
    REQUIRE(val.has_value());
    CHECK(*val == "hello_world");
}

} // namespace ghoti::tests
