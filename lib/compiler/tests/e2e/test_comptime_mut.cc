#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`comptime let mut` reads back its initializer") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime let mut n = 5;
            return n;
        };
    )") == 5);
}

TEST_CASE("`comptime let mut` can be reassigned with `=`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime let mut n = 5;
            n = n + 3;
            return n;
        };
    )") == 8);
}

TEST_CASE("`comptime let mut` can be reassigned with a compound op") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime let mut n = 0;
            n += 3;
            n += 4;
            return n;
        };
    )") == 7);
}

TEST_CASE("`comptime let mut` reassignment in a nested block persists to the outer scope") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime let mut n = 0;
            {
                n = n + 5;
            }
            n = n + 2;
            return n;
        };
    )") == 7);
}

TEST_CASE("a `comptime let mut` initializer must fold") {
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            let mut x: i32 = 5;
            comptime let mut n = x;
            return n;
        };
    )");
}

TEST_CASE("a `comptime let mut` assignment's RHS must fold") {
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            let mut x: i32 = 5;
            comptime let mut n = 0;
            n = x;
            return n;
        };
    )");
}

TEST_CASE("`comptime let mut` reads back an array element") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime let mut arr: [2]i32 = .{1, 2};
            return arr[0] + arr[1];
        };
    )") == 3);
}

TEST_CASE("`comptime let mut` array element is directly assignable") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime let mut arr: [3]i32 = .{1, 2, 3};
            arr[1] = 20;
            return arr[0] + arr[1] + arr[2];
        };
    )") == 24);
}

TEST_CASE("`comptime let mut` array element supports a compound assignment") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime let mut arr: [3]i32 = .{1, 2, 3};
            arr[1] += 100;
            return arr[0] + arr[1] + arr[2];
        };
    )") == 106);
}

TEST_CASE("`comptime let mut` reads back a struct field") {
    CHECK(helpers::compile_and_run(R"(
        const Point = struct { x: i32, y: i32 };
        pub const main = fn(): i32 {
            comptime let mut p = Point{ .x = 1, .y = 2 };
            return p.x + p.y;
        };
    )") == 3);
}

TEST_CASE("`comptime let mut` struct field is directly assignable") {
    CHECK(helpers::compile_and_run(R"(
        const Point = struct { x: i32, y: i32 };
        pub const main = fn(): i32 {
            comptime let mut p = Point{ .x = 1, .y = 2 };
            p.x = 10;
            return p.x + p.y;
        };
    )") == 12);
}

TEST_CASE("`comptime let mut` struct field supports a compound assignment") {
    CHECK(helpers::compile_and_run(R"(
        const Point = struct { x: i32, y: i32 };
        pub const main = fn(): i32 {
            comptime let mut p = Point{ .x = 1, .y = 2 };
            p.x += 9;
            return p.x + p.y;
        };
    )") == 12);
}

TEST_CASE("`comptime let mut` accumulates across `for comptime` iterations") {
    CHECK(helpers::compile_and_run(R"(
        const Point = struct { x: i32, y: i32 };
        pub const main = fn(): i32 {
            comptime let mut p = Point{ .x = 0, .y = 0 };
            for comptime (0..3) |i| { p.x += i; }
            return p.x;
        };
    )") == 3);
}

TEST_CASE("assigning through a chain of fields and elements into a `comptime let mut` aggregate") {
    CHECK(helpers::compile_and_run(R"(
        const Inner = struct { v: i32, xs: [2]i32 };
        const Outer = struct { inner: Inner };
        pub const main = fn(): i32 {
            comptime let mut o = Outer{ .inner = Inner{ .v = 1, .xs = .{ 2, 3 } } };
            o.inner.v = 2;
            o.inner.xs[1] += 5;
            comptime let mut arr: [2]Inner = .{ .{ .v = 1, .xs = .{ 0, 0 } }, .{ .v = 2, .xs = .{ 0, 0 } } };
            arr[1].v *= 7;
            arr[0].xs[0] = 3;
            const k = o.inner.v + o.inner.xs[1] + arr[1].v + arr[0].xs[0];
            return k;
        };
    )") == 2 + 8 + 14 + 3);

    // A union field deeper in the chain, read back at runtime and through a folded call
    CHECK(helpers::compile_and_run(R"(
        const U = union { i: i32, f: f64 };
        const W = struct { u: U, xs: [3]i32 };
        const build = fn(): i32 {
            comptime let mut w: W = .{ .u = .{ .i = 1 }, .xs = .{ 1, 2, 3 } };
            w.u.i += 4;
            w.xs[2] *= 10;
            w.u.f = 2.5;
            return w.xs[2] + @as(i32, @intFromFloat(w.u.f * 2.0));
        };
        const AT = build();
        pub const main = fn(): i32 {
            comptime let mut w: W = .{ .u = .{ .i = 1 }, .xs = .{ 1, 2, 3 } };
            w.u.i += 4;
            w.xs = .{ 7, 8, 9 };
            w.xs[0] -= 1;
            let rt: W = w;
            return rt.xs[0] + rt.u.i + build() + AT;
        };
    )") == 6 + 5 + 35 + 35);

    // Places that can't be known at compile time are still clean errors
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            comptime let mut a: [2][2]i32 = .{ .{ 1, 2 }, .{ 3, 4 } };
            let mut k: usize = 1;
            k += 0;
            a[k][0] = 9;
            return 0;
        };
    )");
    helpers::expect_compile_error(R"(
        const U = union { i: i32, f: f64 };
        const S = struct { u: U };
        pub const main = fn(): i32 {
            comptime let mut s: S = .{ .u = .{ .i = 1 } };
            s.u.f += 1.0;
            return 0;
        };
    )");
}

TEST_CASE("nested `comptime` argument folding resolves mutated `comptime let mut`") {
    CHECK(helpers::compile_and_run(R"(
        const is_digit = fn(comptime c: u8): bool {
            return c >= '0' and c <= '9';
        };
        pub const main = fn(): i32 {
            const s = "a1b2";
            comptime let mut i: usize = 0;
            i += 1;
            if comptime (is_digit(s[i])) {
                return 42;
            }
            return 0;
        };
    )") == 42);
}

TEST_CASE("nested `comptime` argument folding inside `while comptime` condition") {
    CHECK(helpers::compile_and_run(R"(
        const is_digit = fn(comptime c: u8): bool {
            return c >= '0' and c <= '9';
        };
        pub const main = fn(): i32 {
            const s = "123a";
            comptime let mut i: usize = 0;
            while comptime (i < s.len and is_digit(s[i])) : (i += 1) {}
            return @intCast(i32, i);
        };
    )") == 3);
}

TEST_CASE("mutated `comptime let mut` array element visible to nested `comptime` call") {
    CHECK(helpers::compile_and_run(R"(
        const double_val = fn(comptime x: i32): i32 {
            return x * 2;
        };
        pub const main = fn(): i32 {
            comptime let mut arr: [3]i32 = .{10, 20, 30};
            arr[1] = 50;
            const doubled = double_val(arr[1]);
            return doubled;
        };
    )") == 100);
}

TEST_CASE("mutated `comptime let mut` struct field visible to nested `comptime` call") {
    CHECK(helpers::compile_and_run(R"(
        const Point = struct { x: i32, y: i32 };
        const add_pts = fn(comptime a: i32, comptime b: i32): i32 {
            return a + b;
        };
        pub const main = fn(): i32 {
            comptime let mut p = Point{ .x = 10, .y = 20 };
            p.x += 5;
            const sum = add_pts(p.x, p.y);
            return sum;
        };
    )") == 35);
}

TEST_CASE("`for comptime` iterating over string slice in e2e execution") {
    CHECK(helpers::compile_and_run(R"(
        const parse_usize = fn(comptime digits: []u8): usize {
            comptime let mut n: usize = 0;
            for comptime (digits) |c| {
                n = n * 10 + @intCast(usize, c - '0');
            }
            return n;
        };
        pub const main = fn(): i32 {
            const val = parse_usize("42");
            return @intCast(i32, val);
        };
    )") == 42);
}

TEST_CASE("comptime call reaching unreachable fails compilation") {
    helpers::expect_compile_error(R"(
        const bad = fn(): i32 {
            unreachable;
        };
        pub const main = fn(): i32 {
            const x = bad();
            return x;
        };
    )");
}

} // namespace ghoti::tests
