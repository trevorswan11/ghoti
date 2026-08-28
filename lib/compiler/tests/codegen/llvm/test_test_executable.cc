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

TEST_CASE("Test with passing @expect and @require passes") {
    CHECK(helpers::compile_and_run_tests(R"(
        test "assertions pass" {
            @expect(1 + 1 == 2);
            @require(2 + 2 == 4);
        }
    )") == 0);
}

TEST_CASE("Test with failing @require returns failure status without aborting process") {
    CHECK(helpers::compile_and_run_tests(R"(
        test "failing require" {
            @require(1 + 1 == 3);
        }
    )") != 0);
}

TEST_CASE("Test with @src compiles and executes properly") {
    CHECK(helpers::compile_and_run_tests(R"(
        test "src location check" {
            const loc := @src();
            _ = loc;
        }
    )") == 0);
}

} // namespace ghoti::tests
