#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("top-level constexpr block at module level executes at compile time") {
    CHECK(helpers::compile_and_run(R"(
        constexpr {
            const expected := 100;
            @assert(expected == 100);
        }

        pub const main := fn(): i32 {
            return 42;
        };
    )") == 42);
}

TEST_CASE("unlabeled constexpr block in expression position evaluates to void") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const v: void = constexpr {
                const x := 5;
                @assert(x == 5);
            };
            return 0;
        };
    )") == 0);
}

TEST_CASE("labeled constexpr block yields break value (blk: constexpr { ... })") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const r := blk: constexpr {
                const a := 40;
                const b := 2;
                break :blk a + b;
            };
            return r;
        };
    )") == 42);
}

TEST_CASE(
    "labeled constexpr block yields break value with prefix syntax (constexpr blk: { ... })") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const r := constexpr blk: {
                const a := 100;
                const b := 23;
                break :blk a + b;
            };
            return r;
        };
    )") == 123);
}

TEST_CASE("constexpr block with loops and variable mutation") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const sum := blk: constexpr {
                constexpr var total := 0;
                constexpr var i := 1;
                while (i <= 10) : (i += 1) {
                    total += i;
                }
                break :blk total;
            };
            return sum;
        };
    )") == 55);
}

TEST_CASE("nested loops inside labeled constexpr block breaking to inner loop vs outer label") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const val := outer: constexpr {
                constexpr var total := 0;
                constexpr var i := 0;
                while (true) {
                    i += 1;
                    constexpr var j := 0;
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

TEST_CASE("constexpr block non-foldable variable causes compile error") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            var runtime_x: i32 = 10;
            constexpr {
                const y := runtime_x;
            }
            return 0;
        };
    )");
}

TEST_CASE("constexpr block compile-time failure via @compileError causes compile error") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            constexpr {
                @compileError("compile error from constexpr block");
            }
            return 0;
        };
    )");
}

} // namespace ghoti::tests
