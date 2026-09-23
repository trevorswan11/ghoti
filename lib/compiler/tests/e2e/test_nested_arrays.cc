#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("Nested array element stores honor the outer index") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var h: [2]mut [2]mut u8 = undefined;
            h[0][0] = 1;
            h[0][1] = 2;
            h[1][0] = 3;
            h[1][1] = 4;
            return @as(i32, h[0][0]) + @as(i32, h[0][1]) * 10 + @as(i32, h[1][0]) * 100 / 3;
        };
    )") == 121);
}

TEST_CASE("Nested array row stores do not alias") {
    CHECK(helpers::compile_and_run(R"(
        const row := fn(a: u8, b: u8): [2]u8 {
            return [2]u8{a, b};
        };
        pub const main := fn(): i32 {
            var h: [2]mut [2]u8 = undefined;
            h[0] = row(1, 2);
            h[1] = row(3, 4);
            return @as(i32, h[0][0]) + @as(i32, h[0][1]) * 10 + @as(i32, h[1][0]) * 30 +
                   @as(i32, h[1][1]) * 40;
        };
    )") == 271);
}

TEST_CASE("Nested implicit literal initializes a const nested array") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const h: [2][2]u8 = .{.{1, 2}, .{3, 4}};
            return @as(i32, h[1][1]) + 10 * @as(i32, h[0][1]);
        };
    )") == 24);
}

TEST_CASE("Nested literal with explicit inner elements initializes a var nested array") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var h: [2]mut [2]u8 = .{[2]u8{1, 2}, [2]u8{3, 4}};
            return @as(i32, h[1][1]) + 10 * @as(i32, h[0][1]);
        };
    )") == 24);
}

TEST_CASE("Fully typed nested array literal") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const h := [2][2]u8{[2]u8{1, 2}, [2]u8{3, 4}};
            return @as(i32, h[1][1]) + 10 * @as(i32, h[0][1]);
        };
    )") == 24);
}

TEST_CASE("Three dimensional array with loop-indexed stores") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var c: [2]mut [2]mut [2]mut u8 = undefined;
            var n: u8 = 0;
            for (@as(usize, 0)..2) |i| {
                for (@as(usize, 0)..2) |j| {
                    for (@as(usize, 0)..2) |k| {
                        c[i][j][k] = n;
                        n += 1;
                    }
                }
            }
            return @as(i32, c[1][0][1]) * 10 + @as(i32, c[0][1][1]);
        };
    )") == 53);
}

TEST_CASE("Nested array with constexpr dimensions and runtime indices") {
    CHECK(helpers::compile_and_run(R"(
        constexpr N: usize = 3;
        pub const main := fn(): i32 {
            var m: [N]mut [N]mut i32 = undefined;
            for (@as(usize, 0)..N) |i| {
                for (@as(usize, 0)..N) |j| {
                    m[i][j] = @intCast(i * N + j);
                }
            }
            var i: usize = 2;
            return m[i][1] * 10 + m[1][i];
        };
    )") == 75);
}

TEST_CASE("Nested array struct field and lengths") {
    CHECK(helpers::compile_and_run(R"(
        const G := struct { m: [2][3]i32, };
        pub const main := fn(): i32 {
            const g := G{ .m = .{.{1, 2, 3}, .{4, 5, 6}} };
            return g.m[1][2] * 10 + g.m[0][1] + @as(i32, @intCast(g.m.len)) +
                   @as(i32, @intCast(g.m[0].len)) * 2;
        };
    )") == 70);
}

TEST_CASE("Nested array module-scope globals") {
    CHECK(helpers::compile_and_run(R"(
        var grid: [2]mut [2]mut i32 = .{.{1, 2}, .{3, 4}};
        const cg: [2][2]i32 = .{.{5, 6}, .{7, 8}};
        pub const main := fn(): i32 {
            grid[1][0] = 9;
            return grid[1][0] * 10 + grid[0][1] + cg[1][1];
        };
    )") == 100);
}

TEST_CASE("Nested array passed and returned by value") {
    CHECK(helpers::compile_and_run(R"(
        const sum := fn(m: [2][2]i32): i32 {
            return m[0][0] + m[0][1] * 2 + m[1][0] * 3 + m[1][1] * 4;
        };
        const mk := fn(): [2][2]i32 {
            return .{.{1, 2}, .{3, 4}};
        };
        pub const main := fn(): i32 {
            return sum(mk());
        };
    )") == 30);
}

TEST_CASE("For loop over nested array rows") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const m: [2][3]i32 = .{.{1, 2, 3}, .{4, 5, 6}};
            var t: i32 = 0;
            for (m) |row| {
                for (row) |x| {
                    t = t * 2 + x;
                }
            }
            return t - 20;
        };
    )") == 100);
}

TEST_CASE("Nested array row read is a copy") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var h: [2]mut [2]mut u8 = .{.{1, 2}, .{3, 4}};
            const row := h[1];
            h[1][0] = 9;
            return @as(i32, row[0]) + @as(i32, h[1][0]) * 10;
        };
    )") == 93);
}

TEST_CASE("Slice of nested array rows") {
    CHECK(helpers::compile_and_run(R"(
        const total := fn(rows: [][2]u8): i32 {
            var t: i32 = 0;
            for (rows) |r| {
                t = t * 10 + @as(i32, r[0]) + @as(i32, r[1]);
            }
            return t;
        };
        pub const main := fn(): i32 {
            var m: [3]mut [2]u8 = .{.{1, 2}, .{3, 4}, .{0, 1}};
            const s: [][2]u8 = m[0..];
            return total(s[1..]) + @as(i32, s[0][1]);
        };
    )") == 73);
}

TEST_CASE("Nested array mutated in a constexpr function") {
    CHECK(helpers::compile_and_run(R"(
        constexpr f := fn(): i32 {
            var m: [2]mut [2]mut i32 = .{.{1, 2}, .{3, 4}};
            m[1][0] = 7;
            return m[1][0] * 10 + m[0][1];
        };
        pub const main := fn(): i32 {
            constexpr v := f();
            return v;
        };
    )") == 72);
}

} // namespace ghoti::tests
