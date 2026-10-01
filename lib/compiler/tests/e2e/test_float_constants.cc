#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/types.hh>

#include "compiler/sema/error.hh"
#include "ghoti/config.h"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("compile-time f32 arithmetic rounds exactly like runtime f32") {
    CHECK(helpers::compile_and_run(R"(
        const id := fn(x: f32): f32 { return x; };
        pub const main := fn(): i32 {
            const a: f32 = 0.1;
            const b: f32 = 0.2;
            const sum := a + b;
            const quotient := a / b;
            const product := a * b * 3.0;
            if (@bitCast(u32, sum) != @bitCast(u32, id(a) + id(b))) { return 1; }
            if (@bitCast(u32, quotient) != @bitCast(u32, id(a) / id(b))) { return 2; }
            if (@bitCast(u32, product) != @bitCast(u32, id(a) * id(b) * 3.0)) { return 3; }
            const fused := @mulAdd(f32, a, b, 1.0);
            if (@bitCast(u32, fused) != 0x3F828F5C) { return 4; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("f128 constants keep full quad precision") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            let tenth: f128 = 0.1;
            if (@bitCast(u128, tenth) != 0x3FFB999999999999999999999999999A) { return 1; }
            const third: f128 = 1.0 / 3.0;
            if (@bitCast(u128, third) != 0x3FFD5555555555555555555555555555) { return 2; }
            // An untyped constant keeps quad precision until it meets a type
            const wide := 1e300 * 1e300;
            const back := wide / 1e300;
            let narrowed: f64 = back;
            if (narrowed != 1e300) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("a typed literal rounds once, straight from its digits") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            let x: f32 = 1.0000000596046447753906250000000000000001;
            if (@bitCast(u32, x) != 0x3F800001) { return 1; }
            let y: f32 = 1.0000000596046447753906250000000000000000;
            if (@bitCast(u32, y) != 0x3F800000) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@intFromFloat folds across the whole 128-bit range") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const big: u128 = @intFromFloat(u128, 3e38);
            if (big != 300000000000000000000000000000000000000) { return 1; }
            const low: i128 = @intFromFloat(i128, -1.5e38);
            if (low != -150000000000000000000000000000000000000) { return 2; }
            return 0;
        };
    )") == 0);
}

#if GHOTI_ASM_HOST_X86_64
TEST_CASE("f80 constants keep full x87 precision") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            let tenth: f80 = 0.1;
            if (@bitCast(u80, tenth) != 0x3FFBCCCCCCCCCCCCCCCD) { return 1; }
            const third: f80 = 1.0 / 3.0;
            if (@bitCast(u80, third) != 0x3FFDAAAAAAAAAAAAAAAB) { return 2; }
            return 0;
        };
    )") == 0);
}
#endif

TEST_CASE("a typed global built from an untyped constant folds as its declared type") {
    CHECK(helpers::compile_and_run(R"(
        const big := 16777217.0;
        const a: f32 = big;
        const two_hundred := 200;
        const byte: u8 = two_hundred;
        const seven := 7;
        const as_float: f32 = seven;
        let mut as_double: f64 = seven;
        const id := fn(x: f32): f32 { return x; };
        pub const main := fn(): i32 {
            if (@bitCast(u32, a) != 0x4B800000) { return 1; }
            const next := a + 1.0;
            if (@bitCast(u32, next) != @bitCast(u32, id(a) + 1.0)) { return 2; }
            if (@bitCast(i8, byte) != -56) { return 3; }
            if (@bitCast(u32, as_float) != 0x40E00000) { return 4; }
            if (as_double != 7.0) { return 5; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("floats of different widths compare after widening the narrower one") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            let mut narrow: f32 = 0.1;
            let mut wide: f64 = 0.1;
            if (narrow == wide) { return 1; }
            if (!(narrow > wide)) { return 2; }
            // An untyped integer result meeting a float operand converts instead of crashing
            if (@abs(-3) < wide) { return 3; }
            if (!(wide < @abs(-3))) { return 4; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@mulAdd runs at runtime with its operands coerced to T") {
    CHECK(helpers::compile_and_run(R"(
        const id := fn(x: f32): f32 { return x; };
        pub const main := fn(): i32 {
            let r := @mulAdd(f32, id(2.0), id(3.0), 1.0);
            if (r != 7.0) { return 1; }
            let s := @mulAdd(f32, id(2.0), 3, 1);
            if (s != 7.0) { return 2; }
            const folded := @mulAdd(f64, 2, 3, 1);
            if (folded != 7.0) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("nested untyped constant arithmetic meets a typed float") {
    CHECK(helpers::compile_and_run(R"(
        const half := 0.5;
        pub const main := fn(): i32 {
            let mut scale: f32 = 2.0;
            let a := ((half * half) + half) * scale;
            if (a != 1.5) { return 1; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@mulAdd rejects an operand that isn't T") {
    helpers::test_checker_fail(R"(
const f := fn(): void {
    let x := @mulAdd(f64, @as(f128, 1.0), 2.0, 3.0);
};
)",
                               sema::diagnostic{"'@mulAdd' operands must be 'f64'; found 'f128'",
                                                sema::error::OPERATOR_TYPE_MISMATCH,
                                                std::pair{2UZ, 29UZ}});
}

TEST_CASE("builtin operands that round to infinity in their float type are rejected") {
    const auto out_of_range = [](std::string_view value, std::pair<usize, usize> at) {
        return sema::diagnostic{fmt::format("float value {} is out of range for type 'f16'", value),
                                sema::error::LITERAL_OUT_OF_RANGE,
                                at};
    };
    SECTION("a float operand of @mulAdd") {
        helpers::test_checker_fail(R"(
const f := fn(): void {
    let x := @mulAdd(f16, 1.0, 70000.0, 1.0);
};
)",
                                   out_of_range("70000", {2, 31}));
    }
    SECTION("an integer operand of @mulAdd") {
        helpers::test_checker_fail(R"(
const f := fn(): void {
    let x := @mulAdd(f16, 1.0, 164929199106, 1.0);
};
)",
                                   out_of_range("164929199106", {2, 31}));
    }
    SECTION("an integer operand of @as") {
        helpers::test_checker_fail(R"(
const f := fn(): void {
    let x := @as(f16, 70000);
};
)",
                                   out_of_range("70000", {2, 22}));
    }
}

TEST_CASE("a nonzero literal that rounds to zero in its type is rejected") {
    helpers::test_checker_fail(
        R"(
const f := fn(): void {
    let tiny: f32 = 1e-50;
};
)",
        sema::diagnostic{"literal is too small for type 'f32' and would round to zero",
                         sema::error::LITERAL_OUT_OF_RANGE,
                         std::pair{2UZ, 20UZ}});
}

TEST_CASE("A NaN compares unequal to everything, itself included, folded and at runtime") {
    CHECK(helpers::compile_and_run(R"(
        const folded_nan: f32 = @bitCast(f32, @as(u32, 0x7fc00000));
        const folded_ne: bool = folded_nan != folded_nan;
        let mut bits: u32 = 0x7fc00000;
        pub const main := fn(): i32 {
            let nan := @bitCast(f32, bits);
            if (!folded_ne) { return 1; }
            if (!(nan != nan)) { return 2; }
            if (!(nan != 0.0f32)) { return 3; }
            if (nan == nan or nan < 1.0f32 or nan >= 1.0f32) { return 4; }
            return 0;
        };
    )") == 0);
}

} // namespace ghoti::tests
