#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("arbitrary-width integer declarations and arithmetic") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: u3 = 5;
            a = a + 2;                      // 7, fits u3
            var b: i17 = -1000;
            b = b * 3;                      // -3000
            var c: u100 = 1;
            c = c + 41;                     // 42
            if (@as(i32, a) != 7) { return 1; }
            if (@as(i32, b) != -3000) { return 2; }
            if (@as(i32, @as(u64, c)) != 42) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("wide unsigned integer wraps modulo 2^N") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: u3 = 7;
            x = x + 1;                      // wraps to 0
            return @as(i32, x);
        };
    )") == 0);
}

TEST_CASE("@sizeOf of arbitrary-width integers") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            if (@sizeOf(u1) != 1 or @sizeOf(u3) != 1 or @sizeOf(i8) != 1) { return 1; }
            if (@sizeOf(u9) != 2 or @sizeOf(i17) != 4 or @sizeOf(u32) != 4) { return 2; }
            if (@sizeOf(i40) != 8 or @sizeOf(u64) != 8) { return 3; }
            if (@sizeOf(u100) != 16 or @sizeOf(i128) != 16) { return 4; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@bitSizeOf reports a type's exact bit width") {
    CHECK(helpers::compile_and_run(R"(
        const Pair := struct { a: i32, b: i32 };
        pub const main := fn(): i32 {
            if (@bitSizeOf(u1) != 1 or @bitSizeOf(i8) != 8) { return 1; }
            if (@bitSizeOf(u9) != 9 or @bitSizeOf(i17) != 17) { return 2; }
            if (@bitSizeOf(u100) != 100 or @bitSizeOf(i128) != 128) { return 3; }
            if (@bitSizeOf(bool) != 1) { return 4; }
            if (@bitSizeOf(f16) != 16 or @bitSizeOf(f32) != 32 or @bitSizeOf(f64) != 64) {
                return 5;
            }
            // a plain aggregate falls back to @sizeOf(T) * 8
            if (@bitSizeOf(Pair) != @sizeOf(Pair) * 8) { return 6; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@bitSizeOf differs from @sizeOf for sub-byte and odd widths") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            // u9 occupies 2 ABI bytes but only 9 bits
            if (@sizeOf(u9) != 2 or @bitSizeOf(u9) != 9) { return 1; }
            var x: u17 = 3;
            if (@bitSizeOf(@typeOf(x)) != 17) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("arbitrary-width integers widen implicitly to wider same-signedness types") {
    CHECK(helpers::compile_and_run(R"(
        const take_u16 := fn(x: u16): u16 { return x; };
        const take_i64 := fn(x: i64): i64 { return x; };
        pub const main := fn(): i32 {
            const a: u3 = 6;
            const w: u16 = a;                        // u3 -> u16
            const s: i17 = -7;
            const wide: i64 = s;                     // i17 -> i64
            if (@as(i32, take_u16(a)) != 6 or @as(i32, w) != 6) { return 1; }
            if (@as(i32, take_i64(s)) != -7 or @as(i32, wide) != -7) { return 2; }
            return 0;
        };
    )") == 0);
}

} // namespace ghoti::tests
