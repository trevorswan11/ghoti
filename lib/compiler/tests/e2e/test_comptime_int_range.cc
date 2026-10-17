#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

constexpr auto RANGE_EXCEEDED{
    "Compile-time integer result exceeds the 128-bit range of comptime_int"};

auto range_exceeded_at(std::size_t column) -> sema::diagnostic {
    return sema::diagnostic{
        RANGE_EXCEEDED, sema::error::COMPTIME_EVALUATION_FAILED, std::pair{0UZ, column}};
}

} // namespace

TEST_CASE("comptime_int arithmetic past 128 bits is a compile error, not a wrap") {
    helpers::test_checker_fail("const C = (1 << 126) * 4;", range_exceeded_at(11));
    helpers::test_checker_fail("const C = 0xFFFF_FFFF_FFFF_FFFF_FFFF_FFFF_FFFF_FFFF + 1;",
                               range_exceeded_at(10));
    helpers::test_checker_fail("const C = -(1 << 127) - 1;", range_exceeded_at(10));
    helpers::test_checker_fail("const C = 0 - 0xFFFF_FFFF_FFFF_FFFF_FFFF_FFFF_FFFF_FFFF;",
                               range_exceeded_at(10));
    helpers::test_checker_fail("const C = -(1 << 127) / -1 * 2;", range_exceeded_at(10));
}

TEST_CASE("a comptime_int shift past 128 bits names the comptime_int range") {
    helpers::test_checker_fail("const C = 1 << 200;", range_exceeded_at(10));
    helpers::test_checker_fail("const C = 1 << 128;", range_exceeded_at(10));
    helpers::test_checker_fail("const C = 3 << 127;", range_exceeded_at(10));
    helpers::test_checker_fail(
        "const C = 1 << -1;",
        sema::diagnostic{"Negative shift amount in compile-time constant expression",
                         sema::error::COMPTIME_EVALUATION_FAILED,
                         std::pair{0UZ, 10UZ}});
}

TEST_CASE("negating a comptime_int past -2^127 is a compile error") {
    helpers::test_checker_fail("const A = 0x8000_0000_0000_0000_0000_0000_0000_0001; const B = -A;",
                               range_exceeded_at(63));
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
