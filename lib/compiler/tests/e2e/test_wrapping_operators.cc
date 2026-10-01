#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("+% and -% wrap silently at the operand width") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: u8 = 255;
            let mut b: u8 = a +% 1;
            if (@as(i32, b) != 0) { return 1; }
            let mut c: u8 = 0;
            let mut d: u8 = c -% 1;
            if (@as(i32, d) != 255) { return 2; }
            let mut e: i8 = 127;
            let mut f: i8 = e +% 1;
            if (@as(i32, f) != -128) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("*% wraps multiplication at the operand width") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: u4 = 15;
            let mut b: u4 = a *% a;
            return @as(i32, b); // 225 mod 16 == 1
        };
    )") == 1);
}

TEST_CASE("<<% wraps a shift result at the LHS width") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: u8 = 1;
            let mut b: u8 = a <<% 9;
            return @as(i32, b);
        };
    )") == 0);
}

TEST_CASE("prefix -% is well-defined at INT_MIN and matches plain '-' otherwise") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: i8 = 127;
            a +%= 1; // wraps to INT_MIN (-128) without going through a raw literal
            let mut b: i8 = -%a;
            if (@as(i32, b) != -128) { return 1; }
            let mut c: i8 = 5;
            let mut d: i8 = -%c;
            if (@as(i32, d) != -5) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("compound wrapping assignment operators wrap in place") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut x: u8 = 250;
            x +%= 10;
            if (@as(i32, x) != 4) { return 1; }
            let mut y: u8 = 1;
            y <<%= 9;
            if (@as(i32, y) != 0) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("wrapping operators never trap under --runtime-safety") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            // Plain '-a' here would panic under the default runtime-safety-on build.
            let mut a: i8 = 127;
            a +%= 1; // wraps to INT_MIN (-128)
            let mut b: i8 = -%a;
            if (@as(i32, b) != -128) { return 1; }
            let mut c: u8 = 255;
            let mut d: u8 = c +% 1; // plain 'c + 1' here would also panic
            if (@as(i32, d) != 0) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("+% matches the wrapped value @addWithOverflow writes on overflow") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: i32 = 2000000000;
            let mut out: i32 = 0;
            _ = @addWithOverflow(a, a, &mut out);
            let wrapped: i32 = a +% a;
            if (wrapped != out) { return 1; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("<<% matches the wrapped value @shlWithOverflow writes on overflow") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: u8 = 1;
            let mut out: u8 = 0;
            _ = @shlWithOverflow(a, 9u8, &mut out);
            let wrapped: u8 = a <<% 9;
            if (wrapped != out) { return 1; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("comptime_int wrapping operators fold as the plain operator (no wrap)") {
    CHECK(helpers::compile_and_run(R"(
        const c = 200 +% 100;
        pub const main = fn(): i32 { return c / 10; };
    )") == 30);
}

} // namespace ghoti::tests
