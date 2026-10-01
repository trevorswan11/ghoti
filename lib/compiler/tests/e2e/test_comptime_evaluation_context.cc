#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("a comptime block folds calls into functions that take a parameter pack") {
    CHECK(helpers::compile_and_run_tests(R"(
        pub const max := fn(a: auto, b: auto, c...): auto {
            let mut largest := if (a > b) a else b;
            for comptime (c) |arg| {
                if (arg > largest) largest = arg;
            }
            return largest;
        };
        pub const count := fn(c...): usize { return c.len; };
        pub const second := fn(c...): auto { return c[1]; };
        pub const forward := fn(a: auto, c...): auto { return max(a, a, c...); };

        test "max" {
            @expect(max(-4, -123, 45, 78, 90, -23) == 90);
            comptime {
                @assert(max(1, 2) == 2);
                @assert(max(-4, -123, 45) == 45);
                @assert(count() == 0);
                @assert(count(1, 2, 3) == 3);
                @assert(second(7, 8, 9) == 8);
                @assert(forward(4, 9, 1) == 9);
            }
        }
    )") == 0);
}

TEST_CASE("a false compile-time assertion over a pack call is reported") {
    helpers::expect_compile_error(R"(
        pub const count := fn(c...): usize { return c.len; };
        test "t" { comptime { @assert(count(1, 2) == 3); } }
    )");
}

TEST_CASE("a condition-less `if comptime` picks its arm from the evaluation context") {
    CHECK(helpers::compile_and_run_tests(R"(
        pub const where := fn(): i32 { return if comptime 1 else 2; };
        pub const tally := fn(): i32 {
            let mut n: i32 = 10;
            if comptime { n += 5; }
            return n;
        };
        const AT_COMPILE := where();

        test "context" {
            comptime {
                @assert(where() == 1);
                @assert(tally() == 15);
            }
            comptime in_label: {
                @assert(where() == 1);
            }
            @expect(AT_COMPILE == 1);
            let mut at_runtime := where();
            @expect(at_runtime == 2);
            let mut runtime_tally := tally();
            @expect(runtime_tally == 10);
        }
    )") == 0);
}

TEST_CASE("both arms of a condition-less `if comptime` are type checked") {
    helpers::expect_compile_error(R"(
        const f := fn(): i32 { return if comptime 1 else true; };
        pub const main := fn(): i32 { return f(); };
    )");
}

TEST_CASE("a comptime function's params read at compile time are implicitly comptime") {
    CHECK(helpers::compile_and_run_tests(R"(
        pub const min_cx := fn(a: i32, b: i32): i32 {
            return if comptime (a < b) a else b;
        };
        pub const pick := fn(a: auto, b: auto): auto {
            const smaller := if (a < b) a else b;
            return smaller;
        };
        pub const size_of := fn(x: auto): usize {
            const size := @sizeOf(@TypeOf(x));
            return size;
        };

        test "min_cx" {
            comptime {
                @assert(min_cx(1, 2) == 1);
                @assert(min_cx(-4, -123) == -123);
                @assert(pick(9, 3) == 3);
            }
            @expect(min_cx(3, 4) == 3);
            let mut runtime: i64 = 5;
            @expect(size_of(runtime) == 8);
        }
    )") == 0);
}

TEST_CASE("a runtime argument to an implicitly comptime parameter says why it is comptime") {
    CHECK(helpers::raised(R"(
        pub const min_cx := fn(a: i32, b: i32): i32 {
            return if comptime (a < b) a else b;
        };
        const run := fn(x: i32): i32 { return min_cx(x, 4); };
    )",
                          sema::error::COMPTIME_EVALUATION_FAILED));
}

TEST_CASE("params of a `let` closure are never inferred compile-time") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            let min_rt := fn(a: i32, b: i32): i32 {
                return if comptime (a < b) a else b;
            };
            return min_rt(1, 2);
        };
    )");
}

} // namespace ghoti::tests
