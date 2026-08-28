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

TEST_CASE("Custom test runner 'test_runner' is automatically invoked") {
    CHECK(helpers::compile_and_run_tests(R"(
        test "ignored test" {
            @require(false);
        }

        pub const test_runner := fn(): i32 {
            return 77;
        };
    )") == 77);
}

TEST_CASE("Custom test runner 'default_test_runner' is automatically invoked") {
    CHECK(helpers::compile_and_run_tests(R"(
        test "ignored test" {
            @require(false);
        }

        pub const default_test_runner := fn(): i32 {
            return 88;
        };
    )") == 88);
}

TEST_CASE("Custom test runner specified by name receives test metadata slice") {
    CHECK(helpers::compile_and_run_tests(R"(
        pub const Test := struct {
            name: []u8,
            file: []u8,
            line: u32,
            column: u32,
            func: fn(): bool,
        };

        test "test one" {
            @expect(true);
        }

        test "test two" {
            @expect(true);
        }

        pub const my_custom_runner := fn(tests: []Test): i32 {
            if (tests.len == 2) {
                return 42;
            }
            return 1;
        };
    )",
                                         {},
                                         "my_custom_runner") == 42);
}

TEST_CASE("Custom test runner invokes test function pointer directly") {
    CHECK(helpers::compile_and_run_tests(R"(
        pub const Test := struct {
            name: []u8,
            file: []u8,
            line: u32,
            column: u32,
            func: fn(): bool,
        };

        test "successful test" {
            @expect(1 + 1 == 2);
        }

        pub const invoke_runner := fn(tests: []Test): i32 {
            if (tests.len == 1) {
                const ok := tests[0].func();
                if (ok) {
                    return 0;
                }
            }
            return 99;
        };
    )",
                                         {},
                                         "invoke_runner") == 0);
}

} // namespace ghoti::tests
