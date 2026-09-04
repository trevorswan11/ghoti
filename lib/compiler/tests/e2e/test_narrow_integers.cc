#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("i8 / i16 / u16 arithmetic and @sizeOf") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const a: i16 = 300;
            const b: i16 = 44;
            const c: i16 = a + b;                    // 344
            var d: u16 = 60000;
            d = d + 1;                               // 60001
            const e: i8 = -5;
            const f: i8 = 12;
            const total := @as(i32, c) + @as(i32, e) + @as(i32, f) + @as(i32, d);
            if (total != 60352) { return 1; }
            if (@sizeOf(i8) != 1 or @sizeOf(i16) != 2 or @sizeOf(u16) != 2) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("unsigned narrow integer overflow wraps") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var d: u16 = 65535;
            d = d + 1;
            return @as(i32, d);
        };
    )") == 0);
}

TEST_CASE("narrow integers widen implicitly at call sites and returns") {
    CHECK(helpers::compile_and_run(R"(
        const bump := fn(x: i64): i64 { return x + 1; };
        const as_isize := fn(x: i8): isize { return x; };
        pub const main := fn(): i32 {
            const s: i8 = 7;
            const w: i16 = 9;
            const summed := bump(s) + bump(w);                  // i64: 8 + 10 = 18
            return @as(i32, summed) + @as(i32, as_isize(s));    // 18 + 7 = 25
        };
    )") == 25);
}

TEST_CASE("a narrow signed type's exact MIN literal stores and compares at the right width") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: i8 = -128;
            var b: i16 = -32768;
            var c: i32 = -2147483648;
            var d: i4 = -8;
            if (@as(i32, a) != -128) { return 1; }
            if (@as(i32, b) != -32768) { return 2; }
            if (c != -2147483648) { return 3; }
            if (@as(i32, d) != -8) { return 4; }
            return 0;
        };
    )") == 0);
}

} // namespace ghoti::tests
