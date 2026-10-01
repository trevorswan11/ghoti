#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("@intCast sema type checking") {
    SECTION("Valid integer casts succeed") {
        helpers::type_check_and_verify(R"(
            const f := fn(x: usize, y: i64, z: u32): i32 {
                let mut a: i32 = @intCast(i32, x);
                let mut b: u8 = @intCast(u8, y);
                let mut c: i32 = @intCast(i32, z);
                let mut d: usize = @intCast(usize, a);
                return a;
            };
        )");
    }

    SECTION("Non-integer target type is rejected") {
        helpers::test_checker_fail(
            R"(
            const f := fn(x: i32): void {
                let mut y: bool = @intCast(bool, x);
            };
        )",
            sema::diagnostic{"`@intCast` target must be an integer type; found 'bool'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{2UZ, 43UZ}});
    }

    SECTION("Non-integer operand type is rejected") {
        helpers::test_checker_fail(
            R"(
            const f := fn(b: bool): void {
                let mut y: i32 = @intCast(i32, b);
            };
        )",
            sema::diagnostic{"`@intCast` operand must be an integer type; found 'bool'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{2UZ, 47UZ}});
    }

    SECTION("Comptime value out of range fails evaluation") {
        helpers::test_checker_fail(
            R"(
            const x: u8 = @intCast(u8, 300);
        )",
            sema::diagnostic{"Integer value 300 is out of range for target type 'u8' in @intCast",
                             sema::error::COMPTIME_EVALUATION_FAILED,
                             std::pair{1UZ, 39UZ}});
    }

    SECTION("Negative comptime value out of range for unsigned fails evaluation") {
        helpers::test_checker_fail(
            R"(
            const x: u32 = @intCast(u32, -5);
        )",
            sema::diagnostic{"Integer value -5 is out of range for target type 'u32' in @intCast",
                             sema::error::COMPTIME_EVALUATION_FAILED,
                             std::pair{1UZ, 41UZ}});
    }

    SECTION("Fitting comptime value succeeds") {
        helpers::type_check_and_verify(R"(
            const x: u8 = @intCast(u8, 200);
            const y: i32 = @intCast(i32, 1000);
        )");
    }
}

} // namespace ghoti::tests
