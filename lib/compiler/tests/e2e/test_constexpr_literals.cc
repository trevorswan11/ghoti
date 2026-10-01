#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("a comptime_int literal coerces to many concrete widths") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            let n := 200;                 // comptime_int
            let a: u8 = n;
            let b: i16 = n;
            let c: i64 = n;
            let d: u100 = n;
            if (@as(i32, a) != 200) { return 1; }
            if (@as(i32, b) != 200) { return 2; }
            if (@intCast(i32, c) != 200) { return 3; }
            if (@intCast(i32, d) != 200) { return 4; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("constexpr integer arithmetic does not overflow at 32 bits") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            let shifted := 1 << 40;                 // comptime_int, > 2^32
            let product := 1000000 * 1000000;      // 1e12
            let x: i64 = shifted;
            let y: i64 = product;
            if (x != 1099511627776i64) { return 1; }
            if (y != 1000000000000i64) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("an unsuffixed integer literal forces to a float context") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            let a: f32 = 7;               // int literal in a float slot
            let b: f64 = 3;
            if (a != 7.0f32) { return 1; }
            if (b != 3.0f64) { return 2; }
            // comptime_int + comptime_float promotes to comptime_float
            let c := 2 + 0.5;
            let d: f64 = c;
            if (d < 2.49 or d > 2.51) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("a constexpr literal mixed with a concrete operand adopts the concrete type") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            let mut x: u16 = 40000;
            let sum := x + 25000;         // 40000 + 25000 wraps u16 -> 64536 - 65536 ... 65535 max
            if (@as(i32, x + 1000) != 41000) { return 1; }
            let y: i64 = 5;
            let big := y * 100;           // i64
            if (big != 500i64) { return 2; }
            _ = sum;
            return 0;
        };
    )") == 0);
}

TEST_CASE("an un-anchored comptime_int materializes as i32 at runtime") {
    CHECK(helpers::compile_and_run(R"(
        const echo := fn(v: auto): auto { return v; };
        pub const main := fn(): i32 {
            let mut run := 5;                   // var -> i32
            run = run * 3;
            if (run != 15) { return 1; }
            let e := echo(9);             // auto param materializes -> i32
            if (@as(i32, e) != 9) { return 2; }
            if (@bitSizeOf(@TypeOf(run)) != 32) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@TypeOf of an unsuffixed literal is comptime_int / comptime_float") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const I := @TypeOf(0);
            const F := @TypeOf(0.0);
            let a: I = 123;               // comptime_int alias still coerces
            let b: F = 1.5;
            if (@intCast(i32, @as(i64, a)) != 123) { return 1; }
            if (b != 1.5f64) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("a constexpr literal that overflows its target is a compile error") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            let x: u8 = 300;
            return @as(i32, x);
        };
    )");
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            let x: i8 = 128;
            return 0;
        };
    )");
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            let big := 5000;
            let x: u8 = big;
            return 0;
        };
    )");
}

TEST_CASE("negation brings the minimum signed value into range") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            let lo: i8 = -128;
            let hi: i8 = 127;
            return @as(i32, hi) + @as(i32, lo) + 1;   // 127 + (-128) + 1 == 0
        };
    )") == 0);
}

TEST_CASE("`const K := X` aliases a value constant") {
    CHECK(helpers::compile_and_run(R"(
        const X := 42;
        const K := X;
        pub const main := fn(): i32 { return K; };
    )") == 42);
}

} // namespace ghoti::tests
