#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Passing comptime @expect / @require compile and run") {
    CHECK(helpers::compile_and_run_tests(R"(
        const add = fn(a: i32, b: i32): i32 { return a + b; };

        test "comptime block" {
            comptime {
                @expect(add(1, 2) == 3);
                @require(add(1, 2) == 3, "msg");
                const ok = @expect(true);
                @require(ok);
            }
        }
        test "comptime expressions" {
            comptime @expect(add(2, 2) == 4);
            comptime @require(add(2, 2) == 4);
            _ = comptime @expect(true);
        }
        test "local const is compile time" {
            const ok = @expect(add(3, 3) == 6);
            @expect(ok);
        }
    )") == 0);
}

TEST_CASE("A failing comptime @expect is a compile error that keeps evaluating") {
    const auto diags{helpers::type_check_diags(R"(
        test "t" {
            comptime {
                @expect(1 == 2);
                @expect(false, "second");
            }
        }
    )")};
    REQUIRE(diags.codes.size() == 2);
    CHECK(diags.codes[0] == sema::error::STATIC_ASSERTION_FAILED);
    CHECK(diags.messages[0] == "Compile-time expectation failed");
    CHECK(diags.codes[1] == sema::error::STATIC_ASSERTION_FAILED);
    CHECK(diags.messages[1] == "second");
}

TEST_CASE("A failing comptime @require stops evaluating without a second diagnostic") {
    const auto diags{helpers::type_check_diags(R"(
        test "t" {
            comptime {
                @require(1 == 2, "stop");
                @expect(false);
            }
            comptime @require(false);
        }
    )")};
    REQUIRE(diags.codes.size() == 2);
    CHECK(diags.codes[0] == sema::error::STATIC_ASSERTION_FAILED);
    CHECK(diags.messages[0] == "stop");
    CHECK(diags.codes[1] == sema::error::STATIC_ASSERTION_FAILED);
    CHECK(diags.messages[1] == "Compile-time requirement failed");
}

TEST_CASE("A runtime `let` initializer's @expect stays a runtime check") {
    CHECK(helpers::compile_and_run_tests(R"(
        const add = fn(a: i32, b: i32): i32 { return a + b; };
        test "fails at runtime" {
            let r = @expect(add(1, 1) == 3);
            _ = r;
        }
    )") == 1);
}

TEST_CASE("comptime @skip skips its test") {
    CHECK(helpers::compile_and_run_tests(R"(
        let mut reached = false;
        const two = fn(): i32 { return 2; };

        test "skipped from a comptime block" {
            comptime {
                if (two() == 2) { @skip("two"); }
                @expect(false);
            }
            reached = true;
            @require(false);
        }
        test "skipped from a comptime expression" {
            comptime @skip();
            reached = true;
        }
        test "stopped" { @expect(!reached); }
    )") == 0);
}

TEST_CASE("comptime @skip through a `@[testing]` helper skips its test") {
    CHECK(helpers::compile_and_run_tests(R"(
        @[testing]
        const skip_if = fn(c: bool): void {
            if (c) { @skip("helper skip"); }
        };

        test "skipped" {
            comptime skip_if(true);
            @require(false);
        }
        test "not skipped" {
            comptime skip_if(false);
            @expect(true);
        }
        test "not skipped, so it fails" {
            comptime skip_if(false);
            @expect(false);
        }
    )") == 1);
}

TEST_CASE("comptime @expect / @require / @skip are still test-only") {
    CHECK(helpers::raised("const f = fn(): void { comptime @expect(true); };",
                          sema::error::TEST_BUILTIN_OUTSIDE_TEST));
}

} // namespace ghoti::tests
