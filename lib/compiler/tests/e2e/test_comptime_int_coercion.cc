#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("comptime-fits implicit integer coercion runtime execution") {
    SECTION("comptime usize coerces to u8 in const and var decls") {
        CHECK(helpers::compile_and_run(R"(
            const LIMIT: usize = 200;

            pub const main = fn(): i32 {
                const a: u8 = LIMIT;
                let mut b: u8 = LIMIT;
                return @as(i32, a) + @as(i32, b) - 350;
            };
        )") == 50);
    }

    SECTION("comptime usize coerces to u8 in assignment") {
        CHECK(helpers::compile_and_run(R"(
            const LIMIT: usize = 200;

            pub const main = fn(): i32 {
                let mut b: u8 = 0;
                b = LIMIT;
                return @as(i32, b) - 110;
            };
        )") == 90);
    }

    SECTION("comptime usize coerces to u8 in function argument and return") {
        CHECK(helpers::compile_and_run(R"(
            const LIMIT: usize = 120;

            const take_u8 = fn(x: u8): u8 {
                return x;
            };

            const ret_limit = fn(): u8 {
                return LIMIT;
            };

            pub const main = fn(): i32 {
                let r1: u8 = take_u8(LIMIT);
                let r2: u8 = ret_limit();
                return @as(i32, r1) + @as(i32, r2) - 130;
            };
        )") == 110);
    }

    SECTION("comptime integer coerces in struct and array literals") {
        CHECK(helpers::compile_and_run(R"(
            const V1: usize = 10;
            const V2: i64 = 20;

            const S = struct {
                a: u8,
                b: i16,
            };

            pub const main = fn(): i32 {
                let s = S{ .a = V1, .b = V2 };
                let arr: [2]u8 = [2]u8{ V1, 30 };
                return @as(i32, s.a) + @as(i32, s.b) + @as(i32, arr[0]) + @as(i32, arr[1]);
            };
        )") == 70);
    }

    SECTION("comptime positive signed integer coerces to unsigned") {
        CHECK(helpers::compile_and_run(R"(
            const POS: i32 = 42;

            pub const main = fn(): i32 {
                const u: u32 = POS;
                const v: u8 = POS;
                return @as(i32, u) + @as(i32, v);
            };
        )") == 84);
    }

    SECTION("comptime negative integer coerces to narrower signed type that fits") {
        CHECK(helpers::compile_and_run(R"(
            const NEG: i64 = -42;

            pub const main = fn(): i32 {
                const s: i16 = NEG;
                const b: i8 = NEG;
                return @as(i32, s) + @as(i32, b) + 100;
            };
        )") == 16);
    }

    SECTION("comptime-folded binary expression coerces to narrower integer") {
        CHECK(helpers::compile_and_run(R"(
            pub const main = fn(): i32 {
                let x: u8 = 100 + 50;
                return @as(i32, x) - 70;
            };
        )") == 80);
    }

    SECTION("a shift between a comptime_int literal and a concretely-typed comptime local "
            "still coerces back to a comptime_int-returning function's declared return type") {
        CHECK(helpers::compile_and_run(R"(
            const maxUnsigned = fn(T: type): comptime_int {
                const info = @typeInfo(T).int;
                return (1 << info.bits) - 1;
            };

            pub const main = fn(): i32 {
                if (maxUnsigned(u8) == 255 and maxUnsigned(u16) == 65535) { return 42; }
                return 0;
            };
        )") == 42);
    }

    SECTION("a shift amount at or beyond the folding width's bit count does not fold as UB") {
        CHECK(helpers::compile_and_run(R"(
            const maxUnsigned = fn(T: type): comptime_int {
                const info = @typeInfo(T).int;
                return (1 << info.bits) - 1;
            };

            pub const main = fn(): i32 {
                if (maxUnsigned(u64) == 18446744073709551615) { return 42; }
                return 0;
            };
        )") == 42);
    }

    SECTION("negating a type's narrowest representable magnitude does not fold as UB") {
        CHECK(helpers::compile_and_run(R"(
            const minInt = fn(T: type): comptime_int {
                const info = @typeInfo(T).int;
                return if comptime (info.signedness == .signed) -(1 << (info.bits - 1)) else 0;
            };

            pub const main = fn(): i32 {
                if (minInt(i8) == -128 and minInt(i32) == -2147483648 and
                    minInt(i64) == -9223372036854775808) {
                    return 42;
                }
                return 0;
            };
        )") == 42);
    }

    SECTION("negating i128's own narrowest representable magnitude does not fold as UB") {
        CHECK(helpers::compile_and_run(R"(
            const minInt = fn(T: type): comptime_int {
                const info = @typeInfo(T).int;
                return if comptime (info.signedness == .signed) -(1 << (info.bits - 1)) else 0;
            };

            pub const main = fn(): i32 {
                if (minInt(i128) == -170141183460469231731687303715884105728) { return 42; }
                return 0;
            };
        )") == 42);
    }

    SECTION(
        "a chained double dot-access into an explicitly-typed comptime decl does not spuriously "
        "trip the shift-overflow safety check") {
        CHECK(helpers::compile_and_run(R"(
            const maxUnsigned = fn(T: type): comptime_int {
                const bits: u16 = @typeInfo(T).int.bits;
                return (1 << bits) - 1;
            };

            pub const main = fn(): i32 {
                if (maxUnsigned(u16) == 65535 and maxUnsigned(u64) == 18446744073709551615) {
                    return 42;
                }
                return 0;
            };
        )") == 42);
    }
}

} // namespace ghoti::tests
