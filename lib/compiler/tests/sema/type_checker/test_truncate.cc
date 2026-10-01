#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("@truncate sema validation") {
    SECTION("Valid @truncate with narrower target succeeds") {
        helpers::type_check_and_verify(R"(
            pub const test_fn := fn(): void {
                let a: u16 = 0x1234u16;
                let b := @truncate(u8, a);
                let c: u8 = @truncate(a);
            };
        )");
    }

    SECTION("@truncate with non-integer target fails") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(): void {
                let a: u16 = 100u16;
                let b := @truncate(bool, a);
            };
        )",
            sema::diagnostic{"`@truncate` target must be an integer type; found 'bool'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{3UZ, 35UZ}});
    }

    SECTION("@truncate with non-integer operand fails") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(): void {
                let b := @truncate(u8, true);
            };
        )",
            sema::diagnostic{"`@truncate` operand must be an integer type; found 'bool'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{2UZ, 39UZ}});
    }

    SECTION("@truncate with equal width fails with suggestion to use @bitCast or @intCast") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(): void {
                let a: u32 = 100u32;
                let b := @truncate(u32, a);
            };
        )",
            sema::diagnostic{"`@truncate` target type 'u32' has the same width as 'u32'; use "
                             "`@bitCast` or `@intCast` instead",
                             sema::error::TYPE_MISMATCH,
                             std::pair{3UZ, 25UZ}});
    }

    SECTION("@truncate with wider target fails with suggestion to use @as") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(): void {
                let a: u16 = 100u16;
                let b := @truncate(u32, a);
            };
        )",
            sema::diagnostic{"`@truncate` target type 'u32' is wider than 'u16'; use `@as` instead",
                             sema::error::TYPE_MISMATCH,
                             std::pair{3UZ, 25UZ}});
    }

    SECTION("@truncate with untyped integer literal operand fails") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(): void {
                let b := @truncate(u8, 42);
            };
        )",
            sema::diagnostic{"`@truncate` operand must be an integer type; found 'comptime_int'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{2UZ, 39UZ}});
    }
}

} // namespace ghoti::tests
