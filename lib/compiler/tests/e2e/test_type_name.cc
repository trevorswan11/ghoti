#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("`@typeName` of a primitive type") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const s := @typeName(i32);
            return @as(i32, s.len) + @as(i32, s[0]);
        };
    )") == (4 + 'i'));
}

TEST_CASE("`@typeName` of a user struct reports its declared name") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };

        pub const main := fn(): i32 {
            const s := @typeName(Point);
            return @as(i32, s.len) * 10 + @as(i32, s[0]) + @as(i32, s[4]) - 187;
        };
    )") == (6 * 10 + 'P' + 't' - 187));
}

TEST_CASE("`@typeName` of a user enum reports its declared name") {
    CHECK(helpers::compile_and_run(R"(
        const Color := enum { red, green, blue };

        pub const main := fn(): i32 {
            const s := @typeName(Color);
            return @as(i32, s.len) * 10 + @as(i32, s[0]);
        };
    )") == (6 * 10 + 'C'));
}

TEST_CASE("`@typeName` takes the type of a value expression") {
    CHECK(helpers::compile_and_run(R"(
        const Widget := struct { n: i32 };

        pub const main := fn(): i32 {
            var w: Widget = .{ .n = 0 };
            const s := @typeName(@typeOf(w));
            return @as(i32, s.len) * 10 + @as(i32, s[0]) - 100;
        };
    )") == (7 * 10 + 'W' - 100));
}

TEST_CASE("`@typeName` of a pointer type renders structurally") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const s := @typeName(^u8);
            return @as(i32, s.len) + @as(i32, s[0]);
        };
    )") == (4 + '^'));
}

TEST_CASE("`@typeName` renders the `mut` qualifier on a pointer's pointee") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const s := @typeName(^mut u8);
            return @as(i32, s.len)  + @as(i32, s[1]);
        };
    )") == (8 + 'm'));
}

TEST_CASE("`@typeName` of a function type uses `:` before the return type") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const s := @typeName(fn(): i32);
            return @as(i32, s[4]) + @as(i32, s[3]);
        };
    )") == (':' + ')'));
}

} // namespace ghoti::tests
