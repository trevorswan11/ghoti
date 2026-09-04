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
            const total := @as(i32, a) + @as(i32, @as(i16, b)) + @as(i32, d) + @as(i32, e);
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
            return @as(i32, @as(u32, c)) - 256;       // 0
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
            const total := @as(i32, @as(u64, w)) + @as(i32, n) + @as(i32, s) + @as(i32, u);
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
            if (@as(i32, a + @as(f32, b) + @as(f32, c)) != 7) { return 1; }
            return 0;
        };
    )") == 0);
}

} // namespace ghoti::tests
