#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("@min / @max on runtime integers and floats") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: i32 = 17;
            var b: i32 = 25;
            return @min(a, b) + @max(a, b);
        };
    )") == 42);

    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: f64 = 3.5;
            var b: f64 = 41.5;
            return @as(i32, @min(a, b)) + @as(i32, @max(a, b));
        };
    )") == 44);
}

TEST_CASE("@divTrunc / @divFloor / @rem / @mod match their sign conventions") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: i32 = -17;
            var b: i32 = 5;
            // divTrunc = -3, divFloor = -4, rem = -2, mod = 3
            return (@divTrunc(a, b) * -10) + (@divFloor(a, b) * -3) + (-@rem(a, b)) + @mod(a, b);
        };
    )") == 30 + 12 + 2 + 3);
}

TEST_CASE("previously compile-time-only integer bit builtins now lower at runtime") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: u32 = 16u32;
            return @as(i32, @clz(x)) + @as(i32, @ctz(x)) + @as(i32, @popCount(x)) + @abs(-9);
        };
    )") == 27 + 4 + 1 + 9);
}

TEST_CASE("@addWithOverflow / @mulWithOverflow report the flag and write the wrapped value") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: i32 = 6;
            var b: i32 = 7;
            var out: i32 = 0;
            const of := @mulWithOverflow(a, b, &mut out);
            return if (of) 0; else out;
        };
    )") == 42);

    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var big: i32 = 2000000000;
            var out: i32 = 0;
            const of := @addWithOverflow(big, big, &mut out);
            return if (of) 42; else 0;
        };
    )") == 42);
}

TEST_CASE("math builtins still constant-fold") {
    CHECK(helpers::compile_and_run(R"(
        const A := @min(50, 8);
        const B := @divFloor(-7, 2);
        const C := @mod(-7, 2);
        pub const main := fn(): i32 { return A + (-B) * 9 + C; };
    )") == 8 + 36 + 1);
}

} // namespace ghoti::tests
