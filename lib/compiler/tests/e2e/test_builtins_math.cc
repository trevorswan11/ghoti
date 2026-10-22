#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("@min / @max on runtime integers and floats") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: i32 = 17;
            let mut b: i32 = 25;
            return @min(a, b) + @max(a, b);
        };
    )") == 42);

    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: f64 = 3.5;
            let mut b: f64 = 41.5;
            return @intFromFloat(i32, @min(a, b)) + @intFromFloat(i32, @max(a, b));
        };
    )") == 44);
}

TEST_CASE("@divTrunc / @divFloor / @rem / @mod match their sign conventions") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: i32 = -17;
            let mut b: i32 = 5;
            // divTrunc = -3, divFloor = -4, rem = -2, mod = 3
            return (@divTrunc(a, b) * -10) + (@divFloor(a, b) * -3) + (-@rem(a, b)) + @mod(a, b);
        };
    )") == 30 + 12 + 2 + 3);
}

TEST_CASE("@divExact divides operands that divide evenly") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: i32 = -42;
            let mut b: i32 = 6;
            let mut c: u32 = 84u32;
            let mut d: u32 = 4u32;
            return -@divExact(a, b) + @intCast(i32, @divExact(c, d));
        };
    )") == 7 + 21);
}

TEST_CASE("previously compile-time-only integer bit builtins now lower at runtime") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut x: u32 = 16u32;
            return @intCast(i32, @clz(x)) + @intCast(i32, @ctz(x)) + @intCast(i32, @popCount(x)) + @abs(-9);
        };
    )") == 27 + 4 + 1 + 9);
}

TEST_CASE("@addWithOverflow / @mulWithOverflow report the flag and write the wrapped value") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: i32 = 6;
            let mut b: i32 = 7;
            let mut out: i32 = 0;
            let of = @mulWithOverflow(a, b, &mut out);
            return if (of) 0; else out;
        };
    )") == 42);

    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut big: i32 = 2000000000;
            let mut out: i32 = 0;
            let of = @addWithOverflow(big, big, &mut out);
            return if (of) 42; else 0;
        };
    )") == 42);
}

TEST_CASE("layout and target queries take the integer type of their use site") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: i32 = -16;
            const small: u8 = @sizeOf(u64);
            const signed: i8 = @bitSizeOf(u16) - 20;
            const ptr: u16 = @targetPtrBits() / 8;
            return a + @alignOf(u32) + @intCast(i32, small) + signed + @intCast(i32, ptr);
        };
    )") == -16 + 4 + 8 - 4 + 8);
}

TEST_CASE("math builtins still constant-fold") {
    CHECK(helpers::compile_and_run(R"(
        const A = @min(50, 8);
        const B = @divFloor(-7, 2);
        const C = @mod(-7, 2);
        const D = @divExact(-12, 4);
        pub const main = fn(): i32 { return A + (-B) * 9 + C - D; };
    )") == 8 + 36 + 1 + 3);
}

} // namespace ghoti::tests
