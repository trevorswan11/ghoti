#include <catch2/catch_test_macros.hpp>

#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Context-inferred 1-argument casts sema type checking") {
    SECTION("1-arg @intCast in all 5 target contexts") {
        helpers::type_check_and_verify(R"(
            const Point := struct {
                x: i32,
                y: u16,
            };

            const target_fn := fn(val: i32): i32 {
                return val;
            };

            const f := fn(len: usize): i32 {
                // 1. Typed const/var declaration
                let mut a: i32 = @intCast(len);
                let b: u16 = @intCast(len);

                // 2. Assignment RHS
                a = @intCast(len);

                // 3. Call argument
                let r: i32 = target_fn(@intCast(len));

                // 4. Struct initializer field
                let pt := Point{
                    .x = @intCast(len),
                    .y = @intCast(len),
                };

                // 5. Return expression
                return @intCast(len);
            };
        )");
    }

    SECTION("1-arg @as in target contexts") {
        helpers::type_check_and_verify(R"(
            const take64 := fn(x: i64): i64 {
                return x;
            };

            const f := fn(x: i32): i64 {
                let mut a: i64 = @as(x);
                a = @as(x);
                let r: i64 = take64(@as(x));
                return @as(x);
            };
        )");
    }

    SECTION("1-arg @bitCast in target contexts") {
        helpers::type_check_and_verify(R"(
            const take_u32 := fn(x: u32): u32 {
                return x;
            };

            const f := fn(x: i32): u32 {
                let mut a: u32 = @bitCast(x);
                a = @bitCast(x);
                let r: u32 = take_u32(@bitCast(x));
                return @bitCast(x);
            };
        )");
    }

    SECTION("Unconstrained contexts produce clear diagnostic") {
        const auto diags_int = helpers::resolve_diags(R"(
            const f := fn(): void {
                let x := @intCast(42);
            };
        )");
        CHECK(diags_int.message_contains(
            "cannot infer the target type of '@intCast' here; write '@intCast(T, x)'"));

        const auto diags_as = helpers::resolve_diags(R"(
            const f := fn(): void {
                let x := @as(42);
            };
        )");
        CHECK(diags_as.message_contains(
            "cannot infer the target type of '@as' here; write '@as(T, x)'"));

        const auto diags_bit = helpers::resolve_diags(R"(
            const f := fn(): void {
                let x := @bitCast(42);
            };
        )");
        CHECK(diags_bit.message_contains(
            "cannot infer the target type of '@bitCast' here; write '@bitCast(T, x)'"));

        const auto diags_ret_auto = helpers::resolve_diags(R"(
            const f := fn(): auto {
                return @intCast(42);
            };
        )");
        CHECK(diags_ret_auto.message_contains(
            "cannot infer the target type of '@intCast' here; write '@intCast(T, x)'"));
    }

    SECTION("Target validation applies to inferred target") {
        const auto diags = helpers::resolve_diags(R"(
            const f := fn(): void {
                let mut b: bool = @intCast(42);
            };
        )");
        CHECK(diags.message_contains("`@intCast` target must be an integer type; found 'bool'"));
    }

    SECTION("Operand validation applies to 1-arg operand") {
        const auto diags = helpers::resolve_diags(R"(
            const f := fn(b: bool): void {
                let mut x: i32 = @intCast(b);
            };
        )");
        CHECK(diags.message_contains("`@intCast` operand must be an integer type; found 'bool'"));
    }

    SECTION("Constexpr evaluation of 1-arg @intCast") {
        helpers::type_check_and_verify(R"(
            const a: u8 = @intCast(200);
            const b: i32 = @intCast(1000);
        )");

        auto [ctx, idx]{helpers::type_check(R"(
            const x: u8 = @intCast(400);
        )")};
        const auto diags{ctx->root_mod.diagnostics.as_opt<sema::diagnostics>()};
        REQUIRE(diags);
        bool found{false};
        for (const auto& d : *diags) {
            if (d.get_message() &&
                d.get_message()->contains(
                    "Integer value 400 is out of range for target type 'u8' in @intCast")) {
                found = true;
                break;
            }
        }
        CHECK(found);
    }
}

} // namespace ghoti::tests
