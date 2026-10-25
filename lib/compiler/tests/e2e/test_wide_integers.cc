#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("arbitrary-width integer declarations and arithmetic") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut a: u3 = 5;
            a = a + 2;                      // 7, fits u3
            let mut b: i17 = -1000;
            b = b * 3;                      // -3000
            let mut c: u100 = 1;
            c = c + 41;                     // 42
            if (@intCast(i32, a) != 7) { return 1; }
            if (@as(i32, b) != -3000) { return 2; }
            if (@intCast(i32, @intCast(u64, c)) != 42) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("wide unsigned integer wraps modulo 2^N") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut x: u3 = 7;
            x = x + 1;                      // wraps to 0
            return @intCast(i32, x);
        };
    )") == 0);
}

TEST_CASE("@sizeOf of arbitrary-width integers") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
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
        const Pair = struct { a: i32, b: i32 };
        pub const main = fn(): i32 {
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
        pub const main = fn(): i32 {
            // u9 occupies 2 ABI bytes but only 9 bits
            if (@sizeOf(u9) != 2 or @bitSizeOf(u9) != 9) { return 1; }
            let mut x: u17 = 3;
            if (@bitSizeOf(@TypeOf(x)) != 17) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("arbitrary-width integers widen implicitly to wider same-signedness types") {
    CHECK(helpers::compile_and_run(R"(
        const take_u16 = fn(x: u16): u16 { return x; };
        const take_i64 = fn(x: i64): i64 { return x; };
        pub const main = fn(): i32 {
            let a: u3 = 6;
            let w: u16 = a;                        // u3 -> u16
            let s: i17 = -7;
            let wide: i64 = s;                     // i17 -> i64
            if (@intCast(i32, take_u16(a)) != 6 or @intCast(i32, w) != 6) { return 1; }
            if (@intCast(i32, take_i64(s)) != -7 or @intCast(i32, wide) != -7) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("constants wider than 128 bits keep every bit") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let neg_one: i256 = -1;
            let neg_five: i200 = -5;
            let limbs: ^u32 = @ptrCast(^u32, ^neg_one);
            for (0..8) |i| {
                if (limbs[i] != 0xFFFF_FFFF) { return 1 + @intCast(i32, i); }
            }
            if (@bitCast(u200, neg_five) >> 128 != (1 << 72) - 1) { return 10; }
            if (~@as(u2048, 0) >> 2041 != 127) { return 11; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("multiplying and dividing integers past the inline width goes through compiler_rt") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let one: u2048 = 1;
            let a = (one << 1500) + 3;
            let b = (one << 500) + 5;
            let product = a * b;
            // 2^2000 + 5 * 2^1500 + 3 * 2^500 + 15
            let expected = (one << 2000) + (one << 1502) + (one << 1500) + (one << 501) +
                (one << 500) + 15;
            if (product != expected) { return 1; }
            if (product / b != a) { return 2; }
            if ((product + 7) % b != 7) { return 3; }

            // Truncating toward zero, the remainder taking the dividend's sign
            let sa = -@intCast(i2048, a);
            let sb = @intCast(i2048, b);
            let sn = -@intCast(i2048, product + 7);
            if (sn / sb != sa) { return 4; }
            if (sn % sb != -7) { return 5; }
            if (sa * sb != -@intCast(i2048, product)) { return 6; }

            let mut wrapped: u2048 = 0;
            if (!@mulWithOverflow(product, b, ^mut wrapped)) { return 7; }
            if (@mulWithOverflow(a, b, ^mut wrapped) or wrapped != product) { return 8; }
            let max = ~@as(u2048, 0);
            if (product *| b != max) { return 9; }
            let smax = @intCast(i2048, max >> 1);
            if (sa *| @intCast(i2048, product) != -smax - 1) { return 10; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("multiplying and dividing the widest integers compiles promptly") {
    CHECK(helpers::compile_and_run(R"(
        const mul = fn(a: u65535, b: u65535): u65535 { return a * b; };
        const div = fn(a: u65535, b: u65535): u65535 { return a / b; };
        const rem = fn(a: i65535, b: i65535): i65535 { return a % b; };
        pub const main = fn(): i32 {
            let one: u65535 = 1;
            let a = (one << 40000) + 1;
            let b = (one << 20000) + 1;
            let product = mul(a, b);
            if (product != (one << 60000) + (one << 40000) + (one << 20000) + 1) { return 1; }
            if (div(product, b) != a) { return 2; }
            if (rem(-@intCast(i65535, product) - 2, @intCast(i65535, b)) != -2) { return 3; }
            return 0;
        };
    )") == 0);
}

} // namespace ghoti::tests
