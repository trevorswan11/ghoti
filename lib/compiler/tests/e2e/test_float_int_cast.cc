#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("@intFromFloat truncates toward zero") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: f64 = 41.9;
            let mut b: f32 = -2.75f32;
            let x = @intFromFloat(i32, a);
            let y: i8 = @intFromFloat(b);
            if (y != -2i8) { return 1; }
            let mut c: f64 = 255.99;
            let z: u8 = @intFromFloat(c);
            if (z != 255u8) { return 2; }
            return x + 1;
        };
    )") == 42);
}

TEST_CASE("@floatFromInt converts any integer, rounding when inexact") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut n: i64 = -7;
            let f = @floatFromInt(f64, n);
            if (f != -7.0) { return 1; }
            let mut big: u32 = 16777217;
            let g: f32 = @floatFromInt(big);
            if (g != 16777216.0f32) { return 2; }
            let mut u: usize = 3;
            let h: f64 = @floatFromInt(u);
            return @intFromFloat(i32, f * h) + 30;
        };
    )") == 9);
}

TEST_CASE("float/int conversions fold at compile time") {
    CHECK(helpers::compile_and_run(R"(
        const f: f64 = 12.5;
        const i: i32 = @intFromFloat(f);
        const g: f32 = @floatFromInt(7);
        pub const main = fn(): i32 {
            return i + @intFromFloat(i32, g);
        };
    )") == 19);
}

TEST_CASE("an out-of-range or NaN @intFromFloat traps at runtime", "[.panic]") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: f64 = 300.0;
            let b: u8 = @intFromFloat(a);
            return @as(i32, b);
        };
    )") != 0);
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: f32 = -1.0f32;
            let b: u32 = @intFromFloat(a);
            return @intCast(i32, b);
        };
    )") != 0);
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut zero: f64 = 0.0;
            let nan = zero / zero;
            return @intFromFloat(i32, nan);
        };
    )") != 0);
}

TEST_CASE("@intFromFloat accepts values at the edges of the integer range") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut lo: f64 = -128.9;
            let mut hi: f64 = 127.9;
            let mut ulo: f64 = -0.5;
            let a: i8 = @intFromFloat(lo);
            let b: i8 = @intFromFloat(hi);
            let c: u8 = @intFromFloat(ulo);
            return @as(i32, a) + @as(i32, b) + @as(i32, c) + 1;
        };
    )") == 0);
}

TEST_CASE("A cast's operand is typed by the cast, not by the expression around it") {
    CHECK(helpers::compile_and_run(R"(
        let mut one: f32 = 1.0f32;
        const ge: bool = @bitCast(f32, @as(u32, 5)) >= @bitCast(f32, @as(u32, 5));
        pub const main = fn(): i32 {
            if (!ge) { return 1; }
            if (one < @bitCast(f32, @as(u32, 0x3f800000))) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("Float/int conversions fold exactly like they run") {
    CHECK(helpers::compile_and_run(R"(
        const trunc: u8 = @intFromFloat(u8, @as(f32, 3.5));
        const negative_half: u8 = @intFromFloat(u8, @as(f32, -0.5));
        const floor_min: i8 = @intFromFloat(i8, @as(f32, -128.9));
        const overflow: f16 = @floatFromInt(f16, @as(u32, 70000));
        const once: u64 = @bitCast(u64, @floatFromInt(f64, @as(u128, 0xffffffffffffffffffffffffffffffff)));
        pub const main = fn(): i32 {
            if (trunc != 3 or negative_half != 0 or floor_min != -128) { return 1; }
            // Past f16's range is an infinity, as the conversion instruction gives
            if (overflow != @bitCast(f16, @as(u16, 0x7c00))) { return 2; }
            // One rounding straight to f64, not through f128 first
            if (once != 0x47f0000000000000) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@intCast rejects a negative value for u128 and a u128 past a signed range") {
    helpers::expect_compile_error("const a: u128 = @intCast(u128, @bitCast(i8, @as(u8, 128)));");
    helpers::expect_compile_error(
        "const b: i8 = @intCast(i8, @as(u128, 0xfffffffffffffffffffffffffffffffe));");
    helpers::expect_compile_error(
        "const c: i128 = @intCast(i128, @as(u128, 0x80000000000000000000000000000000));");
}

} // namespace ghoti::tests
