#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("Context-inferred 1-argument casts runtime execution", "[context_inferred_casts]") {
    SECTION("1-arg @intCast in 5 contexts") {
        CHECK(helpers::compile_and_run(R"(
            const Point := struct {
                x: i32,
                y: i32,
            };

            const add := fn(a: i32, b: i32): i32 {
                return a + b;
            };

            const compute := fn(n: usize): i32 {
                var a: i32 = @intCast(n);
                a = @intCast(n + 10UZ);

                const pt := Point{
                    .x = @intCast(n),
                    .y = @intCast(n * 2UZ),
                };

                const res: i32 = add(@intCast(n), @intCast(n + 5UZ));
                return a + pt.y + res;
            };

            pub const main := fn(): i32 {
                return compute(5UZ);
            };
        )") == 40);
    }

    SECTION("1-arg @as in return and init") {
        CHECK(helpers::compile_and_run(R"(
            const widen := fn(x: i32): i64 {
                const a: i64 = @as(x);
                return @as(a + 1);
            };

            pub const main := fn(): i32 {
                const r: i64 = widen(41);
                return @intCast(i32, r);
            };
        )") == 42);
    }

    SECTION("1-arg @bitCast in return and init") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                var u: u32 = @bitCast(-42i32);
                var s: i32 = @bitCast(u);
                return s + 42;
            };
        )") == 0);
    }
}

TEST_CASE("Context-inferred 1-argument @intCast out-of-range traps", "[.panic]") {
    SECTION("1-arg @intCast narrowing overflow traps") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                var a: i64 = 3000000000;
                var b: i32 = @intCast(a);
                return b;
            };
        )") != 0);
    }

    SECTION("1-arg @intCast negative to unsigned traps") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                var a: i32 = -1;
                var b: u32 = @intCast(a);
                return @intCast(i32, b);
            };
        )") != 0);
    }
}

} // namespace ghoti::tests
