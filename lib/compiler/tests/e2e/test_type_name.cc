#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("`@typeName` of a primitive type") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const s := @typeName(i32);
            // "i32\0" -> len 4, s[0] == 'i'
            return @as(i32, s.len) * 1000 + @as(i32, s[0]);
        };
    )") == (4 * 1'000 + 'i'));
}

TEST_CASE("`@typeName` of a user struct reports its declared name") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };

        pub const main := fn(): i32 {
            const s := @typeName(Point);
            // "Point\0" -> len 6, s[0] == 'P', s[4] == 't'
            return @as(i32, s.len) * 10000 + @as(i32, s[0]) * 100 + @as(i32, s[4]);
        };
    )") == (6 * 10'000 + 'P' * 100 + 't'));
}

TEST_CASE("`@typeName` of a user enum reports its declared name") {
    CHECK(helpers::compile_and_run(R"(
        const Color := enum { red, green, blue };

        pub const main := fn(): i32 {
            const s := @typeName(Color);
            return @as(i32, s.len) * 100 + @as(i32, s[0]);
        };
    )") == (6 * 100 + 'C'));
}

TEST_CASE("`@typeName` takes the type of a value expression") {
    CHECK(helpers::compile_and_run(R"(
        const Widget := struct { n: i32 };

        pub const main := fn(): i32 {
            var w: Widget = .{ .n = 0 };
            const s := @typeName(@typeOf(w));
            return @as(i32, s.len) * 100 + @as(i32, s[0]);
        };
    )") == (7 * 100 + 'W'));
}

TEST_CASE("`@typeName` of a pointer type renders structurally") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const s := @typeName(^u8);
            // "^u8\0" -> len 4, s[0] == '^'
            return @as(i32, s.len) * 1000 + @as(i32, s[0]);
        };
    )") == (4 * 1'000 + '^'));
}

} // namespace ghoti::tests
