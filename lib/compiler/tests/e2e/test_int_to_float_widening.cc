#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("a fixed-width integer implicitly widens to a float that can represent its full range "
          "exactly") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const x: i32 = 42;
            const y: f64 = x;              // 32-bit source, 53-bit significand: lossless
            if (y != 42.0) { return 1; }

            const u: u32 = 7;
            const v: f64 = u;
            if (v != 7.0) { return 2; }

            const s: i16 = 5;
            const f: f32 = s;              // 16-bit source, 24-bit significand: lossless
            if (f != 5.0) { return 3; }

            const b: u8 = 200;
            const h: f16 = b;              // 8-bit source, 11-bit significand: lossless
            if (h != 200.0) { return 4; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("a negative signed integer widens to a float with the correct sign and magnitude") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const x: i32 = -12345;
            const y: f64 = x;
            if (y != -12345.0) { return 1; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("a 64-bit integer widens to `f80`/`f128`, whose significand covers its full range") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const big: i64 = 9007199254740993; // 2^53 + 1: not exactly representable in f64
            const a: f80 = big;                // f80's 64-bit significand covers it exactly
            if (a != 9007199254740993.0) { return 1; }

            const c: u64 = 18446744073709551615; // u64 max
            const d: f128 = c;                   // f128's 113-bit significand covers it exactly
            if (d != 18446744073709551615.0) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("int-to-float widening works through function parameters and return values") {
    CHECK(helpers::compile_and_run(R"(
        const take := fn(v: f64): f64 { return v * 2.0; };
        const give := fn(): f64 {
            const x: i32 = 21;
            return x;
        };
        pub const main := fn(): i32 {
            const x: i32 = 10;
            if (take(x) != 20.0) { return 1; }
            if (give() != 21.0) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("a `constexpr_int` literal still coerces to any float regardless of width") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const a: f32 = 5;
            const b: f16 = 3;
            if (a != 5.0 or b != 3.0) { return 1; }
            return 0;
        };
    )") == 0);
}

} // namespace ghoti::tests
