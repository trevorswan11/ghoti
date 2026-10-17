#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("a shift amount may be any integer type") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut x: u64 = 256;
            let mut narrow: u6 = 4;
            let mut wide: u128 = 3;
            let mut signed: i8 = 1;
            let a = x >> narrow;   // 16
            let b = x >> wide;     // 32
            let c = x >> signed;   // 128
            let d: u64 = 1 << narrow;
            return @intCast(i32, a + b + c + d);
        };
    )") == 16 + 32 + 128 + 16);
}

TEST_CASE("compound shifts and the wrapping / saturating forms take any amount type") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut x: u8 = 1;
            let mut s: u64 = 3;
            x <<= s;               // 8
            let mut big: u32 = 70;
            let w = x <<% big;     // every bit shifted out
            let sat = x <<| big;   // clamps
            return @as(i32, x) + @as(i32, w) + @as(i32, sat) - 200;
        };
    )") == 8 + 0 + 255 - 200);
}

TEST_CASE("@shlWithOverflow takes a count of any integer type") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut x: u64 = 3;
            let mut s: u6 = 2;
            let mut out: u64 = 0;
            let of = @shlWithOverflow(x, s, &mut out);
            let mut far: u32 = 70;
            let mut gone: u64 = 1;
            let of_far = @shlWithOverflow(x, far, &mut gone);
            return if (of or !of_far or gone != 0) 0; else @intCast(i32, out);
        };
    )") == 12);
}

TEST_CASE("a mixed-type shift folds at compile time") {
    CHECK(helpers::compile_and_run(R"(
        const A: u64 = 1;
        const S: u6 = 5;
        const B: u8 = 200;
        pub const main = fn(): i32 { return @intCast(i32, (A << S) + (B >> @as(u32, 2))); };
    )") == 32 + 50);
}

TEST_CASE("a constant shift amount outside the width is a compile error") {
    helpers::test_checker_fail(
        "const f = fn(x: u64): u64 { return x << 64; };",
        sema::diagnostic{"Shift amount is not less than the 64-bit width of the shifted value",
                         sema::error::COMPTIME_EVALUATION_FAILED,
                         std::pair{0UZ, 35UZ}});
    helpers::test_checker_fail(
        "const f = fn(x: u8): u8 { return x >> 300; };",
        sema::diagnostic{"Shift amount is not less than the 8-bit width of the shifted value",
                         sema::error::COMPTIME_EVALUATION_FAILED,
                         std::pair{0UZ, 33UZ}});
    helpers::test_checker_fail(
        "const f = fn(x: i32): i32 { return x << -1; };",
        sema::diagnostic{"Negative shift amount in compile-time constant expression",
                         sema::error::COMPTIME_EVALUATION_FAILED,
                         std::pair{0UZ, 35UZ}});
}

TEST_CASE("a constant out-of-width amount is fine for <<% and <<|") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut x: u8 = 1;
            return @as(i32, x <<% 300) + @as(i32, x <<| 9);
        };
    )") == 0 + 255);
}

TEST_CASE("a shift amount must still be an integer") {
    helpers::test_checker_fail(
        "const f = fn(x: u64, s: f32): u64 { return x << s; };",
        sema::diagnostic{"Operator '<<' cannot be applied to types 'u64' and 'f32'",
                         sema::error::OPERATOR_TYPE_MISMATCH,
                         std::pair{0UZ, 48UZ}});
}

TEST_CASE("a runtime shift amount outside the width traps", "[.panic]") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 { let mut x: u8 = 1; let mut s: u64 = 257; return @as(i32, x << s); };
    )") != 0);
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 { let mut x: i32 = 1; let mut s: i8 = -1; return x >> s; };
    )") != 0);
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 { let mut x: u64 = 1; let mut s: u32 = 64; return @intCast(i32, x << s); };
    )") != 0);
}

} // namespace ghoti::tests
