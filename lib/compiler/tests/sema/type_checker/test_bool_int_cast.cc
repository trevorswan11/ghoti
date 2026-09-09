#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("@boolFromInt and @intFromBool sema validation") {
    SECTION("Valid @boolFromInt on integer and pointer succeeds") {
        helpers::type_check_and_verify(R"(
            pub const test_fn := fn(): void {
                const a: bool = @boolFromInt(10);
                var x: i32 = 42;
                const b: bool = @boolFromInt(^x);
            };
        )");
    }

    SECTION("@boolFromInt with invalid operand fails") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(): void {
                const b := @boolFromInt(1.5f64);
            };
        )",
            sema::diagnostic{"`@boolFromInt` operand must be an integer or pointer; found 'f64'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{2UZ, 40UZ}});
    }

    SECTION("Valid @intFromBool with explicit and context-inferred type succeeds") {
        helpers::type_check_and_verify(R"(
            pub const test_fn := fn(): void {
                const a := @intFromBool(i32, true);
                const b := @intFromBool(u8, false);
                const c: i64 = @intFromBool(true);
            };
        )");
    }

    SECTION("@intFromBool with non-integer target fails") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(): void {
                const a := @intFromBool(bool, true);
            };
        )",
            sema::diagnostic{"`@intFromBool` target must be an integer type; found 'bool'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{2UZ, 40UZ}});
    }

    SECTION("@intFromBool with non-bool operand fails") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(): void {
                const a := @intFromBool(i32, 42);
            };
        )",
            sema::diagnostic{"`@intFromBool` operand must be of type 'bool'; found 'constexpr_int'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{2UZ, 45UZ}});
    }

    SECTION("@as to bool is rejected with suggestion to use @boolFromInt") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(): void {
                const a := @as(bool, 5);
            };
        )",
            sema::diagnostic{"`@as` cannot convert to `bool`; use `@boolFromInt` instead",
                             sema::error::TYPE_MISMATCH,
                             std::pair{2UZ, 27UZ}});
    }

    SECTION("@as from bool is rejected with suggestion to use @intFromBool") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(): void {
                const a := @as(i32, true);
            };
        )",
            sema::diagnostic{"`@as` cannot convert from `bool`; use `@intFromBool` instead",
                             sema::error::TYPE_MISMATCH,
                             std::pair{2UZ, 27UZ}});
    }
}

} // namespace ghoti::tests
