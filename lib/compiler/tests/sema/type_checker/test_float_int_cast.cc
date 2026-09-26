#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("@intFromFloat and @floatFromInt sema validation") {
    SECTION("Explicit and context-inferred targets succeed") {
        helpers::type_check_and_verify(R"(
            pub const test_fn := fn(f: f32, n: i64): void {
                const a := @intFromFloat(i32, f);
                const b: u8 = @intFromFloat(1.5);
                const c := @floatFromInt(f64, n);
                const d: f32 = @floatFromInt(3);
                const e := @as(f64, 1i32);
            };
        )");
    }

    SECTION("@as from a float to an integer is rejected in favor of @intFromFloat") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(f: f32): void {
                const a := @as(i32, f);
            };
        )",
            sema::diagnostic{
                "`@as` cannot convert a float to an integer; use `@intFromFloat` instead",
                sema::error::TYPE_MISMATCH,
                std::pair{2UZ, 27UZ}});
    }

    SECTION("@as from an integer to a float that can't hold it exactly is rejected") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(n: i64): void {
                const a := @as(f64, n);
            };
        )",
            sema::diagnostic{
                "`@as` cannot convert 'i64' to 'f64' exactly; use `@floatFromInt` instead",
                sema::error::TYPE_MISMATCH,
                std::pair{2UZ, 27UZ}});
    }

    SECTION("@intFromFloat with a non-float operand fails") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(): void {
                const a := @intFromFloat(i32, 1);
            };
        )",
            sema::diagnostic{"`@intFromFloat` operand must be a float; found 'constexpr_int'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{2UZ, 46UZ}});
    }

    SECTION("@floatFromInt with a non-float target fails") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(): void {
                const a := @floatFromInt(i32, 1);
            };
        )",
            sema::diagnostic{"`@floatFromInt` target must be a float type; found 'i32'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{2UZ, 41UZ}});
    }

    SECTION("An out-of-range constant @intFromFloat is a compile error") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(): void {
                const a: u8 = @intFromFloat(256.0);
            };
        )",
            sema::diagnostic{
                "Float value 256 is out of range for target type 'u8' in @intFromFloat",
                sema::error::CONSTEXPR_EVALUATION_FAILED,
                std::pair{2UZ, 44UZ}});
    }
}

} // namespace ghoti::tests
