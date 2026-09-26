#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("@backingInt and @fromBackingInt sema validation") {
    SECTION("Enums, packed aggregates, and tagged unions have backing integers") {
        helpers::type_check_and_verify(R"(
            const E := enum : u8 { x, y };
            const P := packed struct { a: u3, b: u5, };
            const U := union { a: i32, b: f32, };
            pub const test_fn := fn(): void {
                const e: u8 = @backingInt(E.y);
                const f: E = @fromBackingInt(1);
                const g := @fromBackingInt(E, 0u8);
                var p: P = .{ .a = 1, .b = 2 };
                const h: u8 = @backingInt(p);
                const i: P = @fromBackingInt(h);
                var u: U = .{ .a = 1 };
                const j: i32 = @backingInt(u);
            };
        )");
    }

    SECTION("@as from an enum to an integer is rejected in favor of @backingInt") {
        helpers::test_checker_fail(
            R"(
            const E := enum { x, y };
            pub const test_fn := fn(): void {
                const a := @as(i32, E.x);
            };
        )",
            sema::diagnostic{
                "`@as` cannot convert an enum to an integer; use `@backingInt` instead",
                sema::error::TYPE_MISMATCH,
                std::pair{3UZ, 27UZ}});
    }

    SECTION("@as from an integer to an enum is rejected in favor of @fromBackingInt") {
        helpers::test_checker_fail(
            R"(
            const E := enum { x, y };
            pub const test_fn := fn(): void {
                const b := @as(E, 1);
            };
        )",
            sema::diagnostic{
                "`@as` cannot convert an integer to an enum; use `@fromBackingInt` instead",
                sema::error::TYPE_MISMATCH,
                std::pair{3UZ, 27UZ}});
    }

    SECTION("@backingInt of a type with no backing integer fails") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(): void {
                const c := @backingInt(5);
            };
        )",
            sema::diagnostic{
                "`@backingInt` operand must be an enum, a packed struct or union, or a "
                "tagged union; found 'constexpr_int'",
                sema::error::TYPE_MISMATCH,
                std::pair{2UZ, 39UZ}});
    }

    SECTION("@fromBackingInt cannot build a tagged union") {
        helpers::test_checker_fail(
            R"(
            const U := union { a: i32, b: f32, };
            pub const test_fn := fn(): void {
                const d := @fromBackingInt(U, 1);
            };
        )",
            sema::diagnostic{"`@fromBackingInt` target must be an enum or a packed struct or "
                             "union; found 'U'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{3UZ, 43UZ}});
    }

    SECTION("@fromBackingInt rejects an operand wider than the backing integer") {
        helpers::test_checker_fail(
            R"(
            const P := packed struct { a: u8, };
            pub const test_fn := fn(w: u16): void {
                const d := @fromBackingInt(P, w);
            };
        )",
            sema::diagnostic{"`@fromBackingInt` operand must be an integer assignable to 'u8', the "
                             "backing integer of 'P'; found 'u16'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{3UZ, 46UZ}});
    }
}

} // namespace ghoti::tests
