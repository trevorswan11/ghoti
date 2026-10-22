#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("Bit-count builtins read through a reference operand") {
    CHECK(helpers::compile_and_run(R"(
        const clz = fn(x: &u32): u32 { return @clz(x); };
        const bits = fn(x: &mut u32): u32 { return @ctz(x) + @popCount(x); };
        pub const main = fn(): i32 {
            let mut v: u32 = 8;
            return @intCast(i32, clz(&v) + bits(&mut v));
        };
    )") == 32);
}

TEST_CASE("Cast builtins read through a reference operand") {
    CHECK(helpers::compile_and_run(R"(
        const widen = fn(x: &u32): u64 { return @intCast(u64, x); };
        const narrow = fn(x: &u32): u8 { return @truncate(u8, x); };
        const to_float = fn(x: &u32): f32 { return @floatFromInt(f32, x); };
        pub const main = fn(): i32 {
            let v: u32 = 258;
            let f: u32 = @intFromFloat(u32, to_float(&v));
            return @intCast(i32, widen(&v) - 258) + @intCast(i32, narrow(&v)) + @intCast(i32, f - 258);
        };
    )") == 2);
}

TEST_CASE("Math builtins on a reference operand take the referent's type") {
    CHECK(helpers::compile_and_run(R"(
        const abs = fn(x: &i32): auto { return @abs(x); };
        const least = fn(x: &u32): u32 { return @min(x, 3); };
        const root = fn(x: &f64): f64 { return @sqrt(x); };
        pub const main = fn(): i32 {
            let n: i32 = -5;
            let v: u32 = 8;
            let d: f64 = 16.0;
            return abs(&n) + @intCast(i32, least(&v)) + @intFromFloat(i32, root(&d));
        };
    )") == 12);
}

TEST_CASE("Binary operators read through reference operands") {
    CHECK(helpers::compile_and_run(R"(
        const inc = fn(x: &u32): auto { return x + 1; };
        const inc_rev = fn(x: &u32): u32 { return 1 + x; };
        const square = fn(x: &u32, y: &u32): u32 { return x * y; };
        const is_eight = fn(x: &u32): bool { return x == 8; };
        pub const main = fn(): i32 {
            let v: u32 = 8;
            let sum: u32 = inc(&v) + inc_rev(&v) + square(&v, &v);
            let bonus: i32 = if (is_eight(&v)) 1 else 0;
            return @intCast(i32, sum) + bonus;
        };
    )") == 83);
}

} // namespace ghoti::tests
