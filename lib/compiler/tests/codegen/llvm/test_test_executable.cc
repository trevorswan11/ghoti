#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("A test-only source with no pub main compiles and all tests pass") {
    CHECK(helpers::compile_and_run_tests(R"(
        test "always passes" {
            const x := 1 + 1;
            _ = x;
        }

        test "also always passes" {
            const y := 2 + 2;
            _ = y;
        }
    )") == 0);
}

TEST_CASE("A panicking test aborts the test executable") {
    CHECK(helpers::compile_and_run_tests(R"(
        test "boom" {
            @panic("boom");
        }
    )") != 0);
}

} // namespace ghoti::tests
