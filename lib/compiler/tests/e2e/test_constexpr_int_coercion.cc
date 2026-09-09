#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("constexpr-fits implicit integer coercion runtime execution") {
    SECTION("constexpr usize coerces to u8 in const and var decls") {
        CHECK(helpers::compile_and_run(R"(
            constexpr LIMIT: usize = 200;

            pub const main := fn(): i32 {
                const a: u8 = LIMIT;
                var b: u8 = LIMIT;
                return @as(i32, a) + @as(i32, b) - 350;
            };
        )") == 50);
    }

    SECTION("constexpr usize coerces to u8 in assignment") {
        CHECK(helpers::compile_and_run(R"(
            constexpr LIMIT: usize = 200;

            pub const main := fn(): i32 {
                var b: u8 = 0;
                b = LIMIT;
                return @as(i32, b) - 110;
            };
        )") == 90);
    }

    SECTION("constexpr usize coerces to u8 in function argument and return") {
        CHECK(helpers::compile_and_run(R"(
            constexpr LIMIT: usize = 120;

            const take_u8 := fn(x: u8): u8 {
                return x;
            };

            const ret_limit := fn(): u8 {
                return LIMIT;
            };

            pub const main := fn(): i32 {
                const r1: u8 = take_u8(LIMIT);
                const r2: u8 = ret_limit();
                return @as(i32, r1) + @as(i32, r2) - 130;
            };
        )") == 110);
    }

    SECTION("constexpr integer coerces in struct and array literals") {
        CHECK(helpers::compile_and_run(R"(
            constexpr V1: usize = 10;
            constexpr V2: i64 = 20;

            const S := struct {
                a: u8,
                b: i16,
            };

            pub const main := fn(): i32 {
                const s := S{ .a = V1, .b = V2 };
                const arr: [2]u8 = [2]u8{ V1, 30 };
                return @as(i32, s.a) + @as(i32, s.b) + @as(i32, arr[0]) + @as(i32, arr[1]);
            };
        )") == 70);
    }

    SECTION("constexpr positive signed integer coerces to unsigned") {
        CHECK(helpers::compile_and_run(R"(
            constexpr POS: i32 = 42;

            pub const main := fn(): i32 {
                const u: u32 = POS;
                const v: u8 = POS;
                return @as(i32, u) + @as(i32, v);
            };
        )") == 84);
    }

    SECTION("constexpr negative integer coerces to narrower signed type that fits") {
        CHECK(helpers::compile_and_run(R"(
            constexpr NEG: i64 = -42;

            pub const main := fn(): i32 {
                const s: i16 = NEG;
                const b: i8 = NEG;
                return @as(i32, s) + @as(i32, b) + 100;
            };
        )") == 16);
    }

    SECTION("constexpr-folded binary expression coerces to narrower integer") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                const x: u8 = 100 + 50;
                return @as(i32, x) - 70;
            };
        )") == 80);
    }
}

} // namespace ghoti::tests
