#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("@floatCast narrows, widens, and keeps a float at runtime") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var wide: f64 = 0.1;
            const narrow := @floatCast(f32, wide);
            if (@TypeOf(narrow) != f32) { return 1; }
            if (narrow != 0.1f32) { return 2; }

            const inferred: f32 = @floatCast(wide);
            if (inferred != narrow) { return 3; }

            var small: f32 = 1.5f32;
            const widened := @floatCast(f64, small);
            if (@TypeOf(widened) != f64 or widened != 1.5) { return 4; }

            const same := @floatCast(f64, wide);
            if (same != wide) { return 5; }

            // Round to nearest, ties to even: 1 + 2^-24 sits halfway between two f32 values
            var tie: f64 = 1.000000059604644775390625;
            if (@floatCast(f32, tie) != 1.0f32) { return 6; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("a runtime @floatCast overflow rounds to infinity without a check") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var huge: f64 = 1.0e300;
            const up := @floatCast(f32, huge);
            const down := @floatCast(f32, -huge);
            var tiny: f64 = 1.0e-300;
            const zero := @floatCast(f32, tiny);
            if (up != @bitCast(f32, 0x7f800000u32)) { return 1; }
            if (down != @bitCast(f32, 0xff800000u32)) { return 2; }
            if (zero != 0.0f32) { return 3; }

            var nan: f64 = 0.0;
            nan = nan / nan;
            const still_nan := @floatCast(f32, nan);
            if (still_nan == still_nan) { return 4; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@floatCast folds at compile time, rounding once") {
    CHECK(helpers::compile_and_run(R"(
        constexpr wide: f64 = 0.1;
        constexpr narrow: f32 = @floatCast(wide);
        constexpr from_literal := @floatCast(f32, 16777217.0);
        constexpr from_int := @floatCast(f64, 3);
        constexpr inf: f64 = 1.0 / 0.0;
        constexpr kept_inf := @floatCast(f32, inf);
        pub const main := fn(): i32 {
            if (narrow != 0.1f32) { return 1; }
            if (from_literal != 16777216.0f32) { return 2; }
            if (@TypeOf(from_int) != f64 or from_int != 3.0) { return 3; }
            if (kept_inf != @bitCast(f32, 0x7f800000u32)) { return 4; }
            var runtime: f64 = 0.1;
            if (@floatCast(f32, runtime) != narrow) { return 5; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@floatCast sema validation") {
    SECTION("A compile-time finite value that overflows the target is rejected") {
        const auto [ctx, idx]{helpers::expect_compile_error(R"(
            constexpr huge: f64 = 1.0e300;
            constexpr bad := @floatCast(f32, huge);
        )")};
        const auto& diags{ctx->root_mod.diagnostics.as<sema::diagnostics>()};
        CHECK(std::ranges::any_of(diags, [](const auto& diag) {
            return diag.get_error() == sema::error::LITERAL_OUT_OF_RANGE;
        }));
    }

    SECTION("The target must be a float type") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(f: f64): void {
                const a := @floatCast(i32, f);
            };
        )",
            sema::diagnostic{"`@floatCast` target must be a float type; found 'i32'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{2UZ, 38UZ}});
    }

    SECTION("A concrete integer operand points at @floatFromInt") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(n: i32): void {
                const a := @floatCast(f64, n);
            };
        )",
            sema::diagnostic{
                "`@floatCast` operand must be a float; found 'i32'; use `@floatFromInt` instead",
                sema::error::TYPE_MISMATCH,
                std::pair{2UZ, 43UZ}});
    }

    SECTION("A non-numeric operand is rejected") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(b: bool): void {
                const a := @floatCast(f64, b);
            };
        )",
            sema::diagnostic{"`@floatCast` operand must be a float; found 'bool'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{2UZ, 43UZ}});
    }
}

TEST_CASE("@as no longer narrows a float") {
    SECTION("Narrowing a concrete float is rejected in favor of @floatCast") {
        helpers::test_checker_fail(
            R"(
            pub const test_fn := fn(f: f64): void {
                const a := @as(f32, f);
            };
        )",
            sema::diagnostic{"`@as` cannot narrow 'f64' to 'f32'; use `@floatCast`",
                             sema::error::TYPE_MISMATCH,
                             std::pair{2UZ, 27UZ}});
    }

    SECTION("Widening and literal coercion still use @as") {
        helpers::type_check_and_verify(R"(
            pub const test_fn := fn(f: f32): void {
                const a := @as(f64, f);
                const b := @as(f32, 0.1);
                const c := @as(f32, f);
            };
        )");
    }
}

} // namespace ghoti::tests
