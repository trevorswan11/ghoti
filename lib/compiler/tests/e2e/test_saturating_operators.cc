#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("+| and -| clamp unsigned results to the operand range") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: u8 = 250;
            if (@as(i32, a +| 10) != 255) { return 1; }
            if (@as(i32, a +| 5) != 255) { return 2; }
            if (@as(i32, a +| 1) != 251) { return 3; }
            let mut b: u8 = 3;
            if (@as(i32, b -| 10) != 0) { return 4; }
            if (@as(i32, b -| 1) != 2) { return 5; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("+| and -| clamp signed results in both directions") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: i8 = 120;
            if (@as(i32, a +| 100) != 127) { return 1; }
            let mut b: i8 = -120;
            if (@as(i32, b -| 100) != -128) { return 2; }
            if (@as(i32, b +| 100) != -20) { return 3; }
            let mut c: i8 = 100;
            let mut d: i8 = -100;
            if (@as(i32, c -| d) != 127) { return 4; }
            if (@as(i32, d -| c) != -128) { return 5; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("*| clamps products, including sign-mixed ones") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: u8 = 16;
            if (@as(i32, a *| a) != 255) { return 1; }
            let mut b: u8 = 15;
            if (@as(i32, b *| b) != 225) { return 2; }
            let mut c: i16 = 300;
            let mut d: i16 = -300;
            if (@as(i32, c *| c) != 32767) { return 3; }
            if (@as(i32, c *| d) != -32768) { return 4; }
            if (@as(i32, d *| d) != 32767) { return 5; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("<<| saturates, even for shift amounts at or past the width") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: u8 = 3;
            if (@as(i32, a <<| 6) != 192) { return 1; }
            if (@as(i32, a <<| 7) != 255) { return 2; }
            let mut big: u8 = 8;
            if (@as(i32, a <<| big) != 255) { return 3; }
            let mut z: u8 = 0;
            if (@as(i32, z <<| big) != 0) { return 4; }
            let mut s: i8 = -3;
            let mut amount: i8 = 6;
            if (@as(i32, s <<| amount) != -128) { return 5; }
            let mut p: i8 = 1;
            if (@as(i32, p <<| amount) != 64) { return 6; }
            let mut far: i8 = 20;
            if (@as(i32, p <<| far) != 127) { return 7; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("compound saturating assignment clamps in place") {
    CHECK(helpers::compile_and_run(R"(
        const Flags = packed struct { lo: u4, hi: u4, };
        pub const main = fn(): i32 {
            let mut x: u8 = 250;
            x +|= 10;
            if (@as(i32, x) != 255) { return 1; }
            x -|= 255;
            x -|= 1;
            if (@as(i32, x) != 0) { return 2; }
            let mut y: i8 = 64;
            y *|= 4;
            if (@as(i32, y) != 127) { return 3; }
            let mut z: u8 = 1;
            z <<|= 9;
            if (@as(i32, z) != 255) { return 4; }
            let mut f: Flags = .{ .lo = 14, .hi = 0 };
            f.lo +|= 5;
            if (@as(i32, f.lo) != 15) { return 5; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("saturating operators never trap under --runtime-safety") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            // Each plain form here would panic under the default runtime-safety-on build.
            let mut a: i32 = 2147483647;
            if (a +| 1 != a) { return 1; }
            let mut b: i32 = -2147483647 - 1;
            if (b -| 1 != b) { return 2; }
            if (b *| 2 != b) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("saturating operators fold at compile time") {
    CHECK(helpers::compile_and_run(R"(
        const a: u8 = 250;
        const b: u8 = a +| 10;
        const c: i8 = -100;
        const d: i8 = c *| 2;
        const e: u16 = 1;
        const f: u16 = e <<| 40;
        pub const main = fn(): i32 {
            if (@as(i32, b) != 255) { return 1; }
            if (@as(i32, d) != -128) { return 2; }
            if (@as(i32, f) != 65535) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("comptime_int saturating operators fold as the plain operator (no range)") {
    CHECK(helpers::compile_and_run(R"(
        const c = 200 +| 100;
        pub const main = fn(): i32 { return c / 10; };
    )") == 30);
}

} // namespace ghoti::tests
