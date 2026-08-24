#include <catch2/catch_test_macros.hpp>

#include "support/env.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("Environment variables get and set") {
    set_env("GHOTI_TEST_VAR", "hello_world");
    const auto& val{UNWRAP(get_env("GHOTI_TEST_VAR"))};
    CHECK(val == "hello_world");
}

} // namespace ghoti::tests
