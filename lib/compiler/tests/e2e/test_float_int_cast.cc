#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("@intFromFloat truncates toward zero") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: f64 = 41.9;
            var b: f32 = -2.75f32;
            const x := @intFromFloat(i32, a);
            const y: i8 = @intFromFloat(b);
            if (y != -2i8) { return 1; }
            var c: f64 = 255.99;
            const z: u8 = @intFromFloat(c);
            if (z != 255u8) { return 2; }
            return x + 1;
        };
    )") == 42);
}

TEST_CASE("@floatFromInt converts any integer, rounding when inexact") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var n: i64 = -7;
            const f := @floatFromInt(f64, n);
            if (f != -7.0) { return 1; }
            var big: u32 = 16777217;
            const g: f32 = @floatFromInt(big);
            if (g != 16777216.0f32) { return 2; }
            var u: usize = 3;
            const h: f64 = @floatFromInt(u);
            return @intFromFloat(i32, f * h) + 30;
        };
    )") == 9);
}

TEST_CASE("float/int conversions fold at compile time") {
    CHECK(helpers::compile_and_run(R"(
        constexpr f: f64 = 12.5;
        constexpr i: i32 = @intFromFloat(f);
        constexpr g: f32 = @floatFromInt(7);
        pub const main := fn(): i32 {
            return i + @intFromFloat(i32, g);
        };
    )") == 19);
}

TEST_CASE("an out-of-range or NaN @intFromFloat traps at runtime", "[.panic]") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: f64 = 300.0;
            const b: u8 = @intFromFloat(a);
            return @as(i32, b);
        };
    )") != 0);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: f32 = -1.0f32;
            const b: u32 = @intFromFloat(a);
            return @intCast(i32, b);
        };
    )") != 0);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var zero: f64 = 0.0;
            const nan := zero / zero;
            return @intFromFloat(i32, nan);
        };
    )") != 0);
}

TEST_CASE("@intFromFloat accepts values at the edges of the integer range") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var lo: f64 = -128.9;
            var hi: f64 = 127.9;
            var ulo: f64 = -0.5;
            const a: i8 = @intFromFloat(lo);
            const b: i8 = @intFromFloat(hi);
            const c: u8 = @intFromFloat(ulo);
            return @as(i32, a) + @as(i32, b) + @as(i32, c) + 1;
        };
    )") == 0);
}

} // namespace ghoti::tests
