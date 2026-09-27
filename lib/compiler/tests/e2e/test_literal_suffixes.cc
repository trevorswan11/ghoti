#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("width-suffixed integer literals type as their spelled width") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: u8 = 7u8;
            var b: i9 = 1i9;
            var d: isize = 7z;
            var e: usize = 7uz;
            a = a + 1u8;                              // 8
            b = b - 3i9;                              // -2
            const total := @intCast(i32, a) + @intCast(i32, b) + @intCast(i32, d) + @intCast(i32, e);
            if (total != 20) { return 1; }            // 8 + (-2) + 7 + 7
            return 0;
        };
    )") == 0);
}

TEST_CASE("a maximally wide suffixed literal is accepted") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var c: u65535 = 255u65535;
            c = c + 1u65535;
            return @intCast(i32, c) - 256;       // 0
        };
    )") == 0);
}

TEST_CASE("unsuffixed integer literals coerce into wide and sized targets") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const w: u100 = 5;
            const n: u3 = 6;
            const s: isize = 9;
            const u: usize = 4;
            const total := @intCast(i32, w) + @intCast(i32, n) + @intCast(i32, s) + @intCast(i32, u);
            if (total != 24) { return 1; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("float literal width suffixes are accepted") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const a := 1.5f32;
            const b := 2.5f64;
            const c: f64 = 3.0;
            if (@intFromFloat(i32, a + @as(f32, b) + @as(f32, c)) != 7) { return 1; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("hex float literals denote exact binary values") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            if (0x1.8p3 != 12.0) { return 1; }
            if (0x1p-2 != 0.25) { return 2; }
            if (0xA.8 != 10.5) { return 3; }
            if (0x1.fp1f32 != 3.875f32) { return 4; }
            if (0xF_F.8p0 != 255.5) { return 5; }
            const tiny: f64 = 0x1p-1074;
            if (tiny == 0.0) { return 6; }
            if (0x1.fffffffffffffp1023 != 1.7976931348623157e308) { return 7; }
            return 0;
        };
    )") == 0);
}

} // namespace ghoti::tests
