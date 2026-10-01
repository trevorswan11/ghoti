#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("top-level comptime block at module level executes at compile time") {
    CHECK(helpers::compile_and_run(R"(
        comptime {
            let expected := 100;
            @assert(expected == 100);
        }

        pub const main := fn(): i32 {
            return 42;
        };
    )") == 42);
}

TEST_CASE("unlabeled comptime block in expression position evaluates to void") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            let v: void = comptime {
                let x := 5;
                @assert(x == 5);
            };
            return 0;
        };
    )") == 0);
}

TEST_CASE("labeled comptime block yields break value (blk: comptime { ... })") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            let r := blk: comptime {
                let a := 40;
                let b := 2;
                break :blk a + b;
            };
            return r;
        };
    )") == 42);
}

TEST_CASE("labeled comptime block yields break value with prefix syntax (comptime blk: { ... })") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            let r := comptime blk: {
                let a := 100;
                let b := 23;
                break :blk a + b;
            };
            return r;
        };
    )") == 123);
}

TEST_CASE("comptime block with loops and variable mutation") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            let sum := blk: comptime {
                comptime let mut total := 0;
                comptime let mut i := 1;
                while (i <= 10) : (i += 1) {
                    total += i;
                }
                break :blk total;
            };
            return sum;
        };
    )") == 55);
}

TEST_CASE("nested loops inside labeled comptime block breaking to inner loop vs outer label") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            let val := outer: comptime {
                comptime let mut total := 0;
                comptime let mut i := 0;
                while (true) {
                    i += 1;
                    comptime let mut j := 0;
                    while (true) {
                        j += 1;
                        if (j == 5) { break; }
                        total += 1;
                    }
                    if (i == 3) { break :outer total; }
                }
                break :outer 0;
            };
            return val;
        };
    )") == 12);
}

TEST_CASE("comptime block non-foldable variable causes compile error") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            let mut runtime_x: i32 = 10;
            comptime {
                let y := runtime_x;
            }
            return 0;
        };
    )");
}

TEST_CASE("comptime block compile-time failure via @compileError causes compile error") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            comptime {
                @compileError("compile error from comptime block");
            }
            return 0;
        };
    )");
}

} // namespace ghoti::tests
