#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

// An unsuffixed integer / real literal is `constexpr_int` / `constexpr_float`: it is
// compile-time known, coerces to any concrete numeric type it fits, and folds at 128-bit
// (int) / f64 (float) precision before it ever reaches runtime.

TEST_CASE("a constexpr_int literal coerces to many concrete widths") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const n := 200;                 // constexpr_int
            const a: u8 = n;
            const b: i16 = n;
            const c: i64 = n;
            const d: u100 = n;
            if (@as(i32, a) != 200) { return 1; }
            if (@as(i32, b) != 200) { return 2; }
            if (@as(i32, @as(i64, c)) != 200) { return 3; }
            if (@as(i32, @as(u64, d)) != 200) { return 4; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("constexpr integer arithmetic does not overflow at 32 bits") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const shifted := 1 << 40;                 // constexpr_int, > 2^32
            const product := 1000000 * 1000000;      // 1e12
            const x: i64 = shifted;
            const y: i64 = product;
            if (x != 1099511627776i64) { return 1; }
            if (y != 1000000000000i64) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("an unsuffixed integer literal forces to a float context") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const a: f32 = 7;               // int literal in a float slot
            const b: f64 = 3;
            if (a != 7.0f32) { return 1; }
            if (b != 3.0f64) { return 2; }
            // constexpr_int + constexpr_float promotes to constexpr_float
            const c := 2 + 0.5;
            const d: f64 = c;
            if (d < 2.49 or d > 2.51) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("a constexpr literal mixed with a concrete operand adopts the concrete type") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: u16 = 40000;
            const sum := x + 25000;         // 40000 + 25000 wraps u16 -> 64536 - 65536 ... 65535 max
            if (@as(i32, x + 1000) != 41000) { return 1; }
            const y: i64 = 5;
            const big := y * 100;           // i64
            if (big != 500i64) { return 2; }
            _ = sum;
            return 0;
        };
    )") == 0);
}

TEST_CASE("an un-anchored constexpr_int materializes as i32 at runtime") {
    CHECK(helpers::compile_and_run(R"(
        const echo := fn(v: auto): auto { return v; };
        pub const main := fn(): i32 {
            var run := 5;                   // var -> i32
            run = run * 3;
            if (run != 15) { return 1; }
            const e := echo(9);             // auto param materializes -> i32
            if (@as(i32, e) != 9) { return 2; }
            if (@bitSizeOf(@typeOf(run)) != 32) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@typeOf of an unsuffixed literal is constexpr_int / constexpr_float") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const I := @typeOf(0);
            const F := @typeOf(0.0);
            const a: I = 123;               // constexpr_int alias still coerces
            const b: F = 1.5;
            if (@as(i32, @as(i64, a)) != 123) { return 1; }
            if (b != 1.5f64) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("a constexpr literal that overflows its target is a compile error") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            const x: u8 = 300;
            return @as(i32, x);
        };
    )");
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            const x: i8 = 128;
            return 0;
        };
    )");
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            const big := 5000;
            const x: u8 = big;
            return 0;
        };
    )");
}

TEST_CASE("negation brings the minimum signed value into range") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const lo: i8 = -128;
            const hi: i8 = 127;
            return @as(i32, hi) + @as(i32, lo) + 1;   // 127 + (-128) + 1 == 0
        };
    )") == 0);
}

} // namespace ghoti::tests
