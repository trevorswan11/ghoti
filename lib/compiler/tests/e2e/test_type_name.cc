#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view SHAPES{R"(
    pub const Point := struct { pub x: i32, };
    pub const Color := enum { red, green, };
    const Hidden := struct { pub n: u8, };
    pub const hidden := fn(): Hidden { return .{ .n = 1 }; };
)"};

} // namespace

TEST_CASE("`@typeName` of a primitive type") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const s := @typeName(i32);
            return @intCast(i32, s.len) + @as(i32, s[0]);
        };
    )") == (4 + 'i'));
}

TEST_CASE("`@typeName` of a user struct reports its declared name") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };

        pub const main := fn(): i32 {
            const s := @typeName(Point);
            return @intCast(i32, s.len) * 10 + @as(i32, s[0]) + @as(i32, s[4]) - 187;
        };
    )") == (6 * 10 + 'P' + 't' - 187));
}

TEST_CASE("`@typeName` of a user enum reports its declared name") {
    CHECK(helpers::compile_and_run(R"(
        const Color := enum { red, green, blue };

        pub const main := fn(): i32 {
            const s := @typeName(Color);
            return @intCast(i32, s.len) * 10 + @as(i32, s[0]);
        };
    )") == (6 * 10 + 'C'));
}

TEST_CASE("`@typeName` takes the type of a value expression") {
    CHECK(helpers::compile_and_run(R"(
        const Widget := struct { n: i32 };

        pub const main := fn(): i32 {
            var w: Widget = .{ .n = 0 };
            const s := @typeName(@TypeOf(w));
            return @intCast(i32, s.len) * 10 + @as(i32, s[0]) - 100;
        };
    )") == (7 * 10 + 'W' - 100));
}

TEST_CASE("`@typeName` of a pointer type renders structurally") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const s := @typeName(^u8);
            return @intCast(i32, s.len) + @as(i32, s[0]);
        };
    )") == (4 + '^'));
}

TEST_CASE("`@typeName` renders the `mut` qualifier on a pointer's pointee") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const s := @typeName(^mut u8);
            return @intCast(i32, s.len)  + @as(i32, s[1]);
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

TEST_CASE("`@typeName` of an imported aggregate reports its declared name") {
    CHECK(helpers::compile_and_run(
              R"(
            import "shapes.gh" as shapes;

            const same := fn(a: []u8, e: []u8): bool {
                if (a.len != e.len) { return false; }
                for (0..a.len) |i| { if (a[i] != e[i]) { return false; } }
                return true;
            };

            pub const main := fn(): i32 {
                const p := @typeName(shapes.Point);
                if (!same(p[0..p.len], "Point")) { return 1; }
                const c := @typeName(shapes.Color);
                if (!same(c[0..c.len], "Color")) { return 2; }
                const h := @typeName(@TypeOf(shapes.hidden()));
                if (!same(h[0..h.len], "Hidden")) { return 3; }
                return 0;
            };
        )",
              {helpers::mock_file{"shapes.gh", SHAPES, "shapes"}}) == 0);
}

TEST_CASE("`@typeName` names user aggregates nested inside compound types") {
    CHECK(helpers::compile_and_run(
              R"(
            import "shapes.gh" as shapes;
            const Local := struct { n: u8, };

            const same := fn(a: []u8, e: []u8): bool {
                if (a.len != e.len) { return false; }
                for (0..a.len) |i| { if (a[i] != e[i]) { return false; } }
                return true;
            };

            pub const main := fn(): i32 {
                const p := @typeName(^shapes.Point);
                if (!same(p[0..p.len], "^Point")) { return 1; }
                const a := @typeName([2]Local);
                if (!same(a[0..a.len], "[2]Local")) { return 2; }
                const f := @typeName(fn(c: shapes.Color): Local);
                if (!same(f[0..f.len], "fn(Color): Local")) { return 3; }
                const s := @typeName([]mut shapes.Point);
                if (!same(s[0..s.len], "[]mut Point")) { return 4; }
                return 0;
            };
        )",
              {helpers::mock_file{"shapes.gh", SHAPES, "shapes"}}) == 0);
}

} // namespace ghoti::tests
