#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

constexpr auto RANGE_EXCEEDED{
    "Compile-time integer result exceeds the 16777216-bit limit of comptime_int"};

} // namespace

TEST_CASE("comptime_int arithmetic past 128 bits stays exact") {
    CHECK(helpers::compile_and_run(R"(
        const A = (1 << 126) * 4;
        const B = 0xFFFF_FFFF_FFFF_FFFF_FFFF_FFFF_FFFF_FFFF + 1;
        const C = -(1 << 127) - 1;
        const D = 0 - 0xFFFF_FFFF_FFFF_FFFF_FFFF_FFFF_FFFF_FFFF;
        const E = 3 << 127;
        const F = 1 << 200;
        pub const main = fn(): i32 {
            const exact = A == 1 << 128 and B == A and C + 1 == -(1 << 127) and D == 1 - B and
                E / 3 == 1 << 127 and F >> 199 == 2 and -F < C and F % 7 == 4 and
                (F | 5) & 7 == 5 and (-F ^ F) < 0 and ~F == -F - 1;
            return if (exact) 3 else 2;
        };
    )") == 3);
}

TEST_CASE("a runaway comptime_int is a compile error") {
    helpers::test_checker_fail("const C = 1 << (1 << 40);",
                               sema::diagnostic{RANGE_EXCEEDED,
                                                sema::error::COMPTIME_EVALUATION_FAILED,
                                                std::pair{0UZ, 10UZ}});
    helpers::test_checker_fail(
        "const C = 1 << -1;",
        sema::diagnostic{"Negative shift amount in compile-time constant expression",
                         sema::error::COMPTIME_EVALUATION_FAILED,
                         std::pair{0UZ, 10UZ}});
}

TEST_CASE("a comptime_int literal may be any width") {
    CHECK(helpers::compile_and_run(R"(
        const H = 0x1FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF;
        const D = 115792089237316195423570985008687907853269984665640564039457584007913129639935;
        pub const main = fn(): i32 { return if (H == (1 << 129) - 1 and D + 1 == 1 << 256) 0 else 1; };
    )") == 0);
    helpers::expect_compile_error("pub const main = fn(): i32 { let x = "
                                  "0x1FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFu64; return 0; };");
}

TEST_CASE("an integer type wider than 128 bits holds a wide comptime_int") {
    CHECK(helpers::compile_and_run(R"(
        const X: u256 = (1 << 200) + 5;
        pub const main = fn(): i32 {
            let y: u256 = X;
            let top: u256 = 1 << 255;
            const folded = @max(0, 1 << 140) > 1 << 139;
            return @intCast(@truncate(u8, y) + @truncate(u8, y >> 200) + @truncate(u8, top >> 250) +
                            @as(u8, @intFromBool(folded)));
        };
    )") == 39);
    helpers::expect_compile_error("const X: u256 = 1 << 256; pub const main = fn(): i32 { return "
                                  "@intCast(@truncate(u8, X)); };");
}

TEST_CASE("comptime_int arithmetic is exact across its whole range") {
    CHECK(helpers::compile_and_run(R"(
        const MAX = (1 << 127) + ((1 << 127) - 1);
        const MIN = -(1 << 127);
        const TOP = (MAX - (1 << 127)) >> 120;
        pub const main = fn(): i32 {
            const exact = MAX - MAX == 0 and MAX > 0 and MIN < 0 and TOP == 127 and
                MIN / -1 == 1 << 127 and -MIN == 1 << 127 and (-1 << 127) == MIN and
                MIN >> 200 == -1 and -7 >> 1 == -4 and -7 % 2 == -1 and 0 << 300 == 0 and
                MAX + MIN == (1 << 127) - 1;
            return if (exact) 3; else 2;
        };
    )") == 3);
}

TEST_CASE("a suffixed literal must fit its own type") {
    CHECK(helpers::raised("const a = 300u8;", sema::error::LITERAL_OUT_OF_RANGE));
    CHECK(helpers::raised("const a = 128i8;", sema::error::LITERAL_OUT_OF_RANGE));
    CHECK(helpers::raised("const a = -129i8;", sema::error::LITERAL_OUT_OF_RANGE));
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 { const lo = -128i8; const hi = 255u8; return if (lo < 0 and hi == 255) 0 else 1; };
    )") == 0);
}

TEST_CASE("concrete integer overflow keeps its type-named messages") {
    helpers::test_checker_fail(
        "const C: u64 = 1 << 64;",
        sema::diagnostic{"Shift amount is not less than the 64-bit width of the shifted value",
                         sema::error::COMPTIME_EVALUATION_FAILED,
                         std::pair{0UZ, 15UZ}});
    helpers::test_checker_fail(
        "const C: i8 = 100; const D = C * 2;",
        sema::diagnostic{"Signed integer overflow in compile-time constant expression: the result "
                         "does not fit 'i8'",
                         sema::error::COMPTIME_EVALUATION_FAILED,
                         std::pair{0UZ, 29UZ}});
}

} // namespace ghoti::tests
