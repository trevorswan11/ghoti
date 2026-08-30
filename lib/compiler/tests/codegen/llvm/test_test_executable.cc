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

TEST_CASE("A non-weak `test_runner` overrides the builtin weak default") {
    CHECK(helpers::compile_and_run_tests(R"(
        test "ignored test" {
            @require(false);
        }

        pub const test_runner := fn(tests: []builtin::Test): i32 {
            _ = tests;
            return 77;
        };
    )") == 77);
}

TEST_CASE("The overriding `test_runner` receives the test metadata slice") {
    CHECK(helpers::compile_and_run_tests(R"(
        test "test one" {
            @expect(true);
        }

        test "test two" {
            @expect(true);
        }

        pub const test_runner := fn(tests: []builtin::Test): i32 {
            if (tests.len == 2) {
                return 42;
            }
            return 1;
        };
    )") == 42);
}

TEST_CASE("The overriding `test_runner` invokes a test function pointer directly") {
    CHECK(helpers::compile_and_run_tests(R"(
        test "successful test" {
            @expect(1 + 1 == 2);
        }

        pub const test_runner := fn(tests: []builtin::Test): i32 {
            if (tests.len == 1) {
                const ok := tests[0].func();
                if (ok) {
                    return 0;
                }
            }
            return 99;
        };
    )") == 0);
}

TEST_CASE("The default runner counts failing tests") {
    CHECK(helpers::compile_and_run_tests(R"(
        test "pass" { @expect(true); }
        test "fail one" { @expect(false); }
        test "fail two" { @require(1 == 2); }
    )") == 2);
}

TEST_CASE("@skip short-circuits a test and it does not count as a failure") {
    CHECK(helpers::compile_and_run_tests(R"(
        test "skipped" {
            @skip("not ready");
            @require(false);
        }
        test "also skipped, no message" {
            @skip();
            @expect(1 == 2);
        }
        test "runs" { @expect(true); }
    )") == 0);
}

TEST_CASE("@skip in one test does not hide a real failure in another") {
    CHECK(helpers::compile_and_run_tests(R"(
        test "skipped" { @skip("later"); }
        test "genuinely fails" { @require(false); }
    )") == 1);
}

} // namespace ghoti::tests
