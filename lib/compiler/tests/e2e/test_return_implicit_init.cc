#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("`return .{...}` infers the struct type from the return type") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };

        const make := fn(): Point {
            return .{ .x = 40, .y = 2 };
        };

        pub const main := fn(): i32 {
            const p := make();
            return p.x + p.y;
        };
    )") == 42);
}

TEST_CASE("`return .{...}` works in both arms of an `if` expression") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };

        const pick := fn(hi: bool): Point {
            return if (hi) .{ .x = 30, .y = 12 }; else .{ .x = 1, .y = 1 };
        };

        pub const main := fn(): i32 {
            const p := pick(true);
            return p.x + p.y;
        };
    )") == 42);
}

TEST_CASE("`return .{...}` works in a `match` arm value") {
    CHECK(helpers::compile_and_run(R"(
        const Tag := enum { a, b };
        const Point := struct { x: i32, y: i32 };

        const from_tag := fn(t: Tag): Point {
            return match (t) {
                .a => .{ .x = 40, .y = 2 },
                .b => .{ .x = 0, .y = 0 },
            };
        };

        pub const main := fn(): i32 {
            const p := from_tag(.a);
            return p.x + p.y;
        };
    )") == 42);
}

TEST_CASE("`return` with a bare `.variant` picks up an enum return type") {
    CHECK(helpers::compile_and_run(R"(
        const Tag := enum { a, b, c };

        const third := fn(): Tag {
            return .c;
        };

        pub const main := fn(): i32 {
            return match (third()) {
                .a => 1,
                .b => 2,
                .c => 42,
            };
        };
    )") == 42);
}

TEST_CASE("`return .{...}` for a method returning `@this()`") {
    CHECK(helpers::compile_and_run(R"(
        const Counter := struct {
            value: i32,

            const zero := fn(): @this() {
                return .{ .value = 0 };
            };

            const bumped := fn(^self, by: i32): @this() {
                return .{ .value = self.value + by };
            };
        };

        pub const main := fn(): i32 {
            const c := Counter.zero().bumped(42);
            return c.value;
        };
    )") == 42);
}

} // namespace ghoti::tests
