#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("width-suffixed integer literals type as their spelled width") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: u8 = 7u8;
            let mut b: i9 = 1i9;
            let mut d: isize = 7z;
            let mut e: usize = 7uz;
            a = a + 1u8;                              // 8
            b = b - 3i9;                              // -2
            let total = @intCast(i32, a) + @intCast(i32, b) + @intCast(i32, d) + @intCast(i32, e);
            if (total != 20) { return 1; }            // 8 + (-2) + 7 + 7
            return 0;
        };
    )") == 0);
}

TEST_CASE("a maximally wide suffixed literal is accepted") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut c: u65535 = 255u65535;
            c = c + 1u65535;
            return @intCast(i32, c) - 256;       // 0
        };
    )") == 0);
}

TEST_CASE("unsuffixed integer literals coerce into wide and sized targets") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let w: u100 = 5;
            let n: u3 = 6;
            let s: isize = 9;
            let u: usize = 4;
            let total = @intCast(i32, w) + @intCast(i32, n) + @intCast(i32, s) + @intCast(i32, u);
            if (total != 24) { return 1; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("float literal width suffixes are accepted") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let a = 1.5f32;
            let b = 2.5f64;
            let c: f64 = 3.0;
            if (@intFromFloat(i32, a + @floatCast(f32, b) + @floatCast(f32, c)) != 7) { return 1; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("hex float literals denote exact binary values") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            if (0x1.8p3 != 12.0) { return 1; }
            if (0x1p-2 != 0.25) { return 2; }
            if (0xA.8 != 10.5) { return 3; }
            if (0x1.fp1f32 != 3.875f32) { return 4; }
            if (0xF_F.8p0 != 255.5) { return 5; }
            let tiny: f64 = 0x1p-1074;
            if (tiny == 0.0) { return 6; }
            if (0x1.fffffffffffffp1023 != 1.7976931348623157e308) { return 7; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("float literals at the edge of their type's range stay finite") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            // Both round down to the type's max rather than up to infinity
            let h: f16 = 65519.0;
            let f: f32 = 3.4028235e38;
            if (h != 65504.0) { return 1; }
            if (f / f != 1.0) { return 2; }
            let n: f16 = -65519;
            if (n != -65504.0) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("integer literals past i128 max keep their unsigned value") {
    CHECK(helpers::compile_and_run(R"(
        const BIG = 340282366920938463463374607431768211455;
        pub const main = fn(): i32 {
            if (BIG < 0) { return 1; }
            if (340282366920938463463374607431768211455 < 0) { return 2; }
            const as_u: u128 = BIG;
            if (as_u != 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF) { return 3; }
            if (BIG / 2 != 170141183460469231731687303715884105727) { return 4; }
            const as_f: f64 = BIG;
            if (as_f < 3e38) { return 5; }
            let lit_f: f64 = 340282366920938463463374607431768211455;
            if (lit_f < 3e38) { return 6; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("a negated suffixed literal can be its type's minimum") {
    CHECK(helpers::compile_and_run(R"(
        const min8: i8 = -128i8;
        pub const main = fn(): i32 {
            let min16 = -32768i16;
            if (min8 != -128 or min16 != -32768) { return 1; }
            return 0;
        };
    )") == 0);
}

} // namespace ghoti::tests
