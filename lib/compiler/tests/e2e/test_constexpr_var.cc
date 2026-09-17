#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`constexpr var` reads back its initializer") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var n := 5;
            return n;
        };
    )") == 5);
}

TEST_CASE("`constexpr var` can be reassigned with `=`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var n := 5;
            n = n + 3;
            return n;
        };
    )") == 8);
}

TEST_CASE("`constexpr var` can be reassigned with a compound op") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var n := 0;
            n += 3;
            n += 4;
            return n;
        };
    )") == 7);
}

TEST_CASE("`constexpr var` reassignment in a nested block persists to the outer scope") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var n := 0;
            {
                n = n + 5;
            }
            n = n + 2;
            return n;
        };
    )") == 7);
}

TEST_CASE("a `constexpr var` initializer must fold") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            constexpr var n := x;
            return n;
        };
    )");
}

TEST_CASE("a `constexpr var` assignment's RHS must fold") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            constexpr var n := 0;
            n = x;
            return n;
        };
    )");
}

TEST_CASE("`constexpr var` reads back an array element") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var arr: [2]i32 = .{1, 2};
            return arr[0] + arr[1];
        };
    )") == 3);
}

TEST_CASE("`constexpr var` array element is directly assignable") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var arr: [3]i32 = .{1, 2, 3};
            arr[1] = 20;
            return arr[0] + arr[1] + arr[2];
        };
    )") == 24);
}

TEST_CASE("`constexpr var` array element supports a compound assignment") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var arr: [3]i32 = .{1, 2, 3};
            arr[1] += 100;
            return arr[0] + arr[1] + arr[2];
        };
    )") == 106);
}

TEST_CASE("`constexpr var` reads back a struct field") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            constexpr var p := Point{ .x = 1, .y = 2 };
            return p.x + p.y;
        };
    )") == 3);
}

TEST_CASE("`constexpr var` struct field is directly assignable") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            constexpr var p := Point{ .x = 1, .y = 2 };
            p.x = 10;
            return p.x + p.y;
        };
    )") == 12);
}

TEST_CASE("`constexpr var` struct field supports a compound assignment") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            constexpr var p := Point{ .x = 1, .y = 2 };
            p.x += 9;
            return p.x + p.y;
        };
    )") == 12);
}

TEST_CASE("`constexpr var` accumulates across `for constexpr` iterations") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            constexpr var p := Point{ .x = 0, .y = 0 };
            for constexpr (0..3) |i| { p.x += i; }
            return p.x;
        };
    )") == 3);
}

TEST_CASE("assigning more than one level into a `constexpr var` aggregate is a clean error") {
    helpers::expect_compile_error(R"(
        const Inner := struct { v: i32 };
        const Outer := struct { inner: Inner };
        pub const main := fn(): i32 {
            constexpr var o := Outer{ .inner = Inner{ .v = 1 } };
            o.inner.v = 2;
            return o.inner.v;
        };
    )");
}

TEST_CASE("nested `constexpr` argument folding resolves mutated `constexpr var`") {
    CHECK(helpers::compile_and_run(R"(
        constexpr is_digit := fn(constexpr c: u8): bool {
            return c >= '0' and c <= '9';
        };
        pub const main := fn(): i32 {
            constexpr s := "a1b2";
            constexpr var i: usize = 0;
            i += 1;
            if constexpr (is_digit(s[i])) {
                return 42;
            }
            return 0;
        };
    )") == 42);
}

TEST_CASE("nested `constexpr` argument folding inside `while constexpr` condition") {
    CHECK(helpers::compile_and_run(R"(
        constexpr is_digit := fn(constexpr c: u8): bool {
            return c >= '0' and c <= '9';
        };
        pub const main := fn(): i32 {
            constexpr s := "123a";
            constexpr var i: usize = 0;
            while constexpr (i < s.len and is_digit(s[i])) : (i += 1) {}
            return @intCast(i32, i);
        };
    )") == 3);
}

TEST_CASE("mutated `constexpr var` array element visible to nested `constexpr` call") {
    CHECK(helpers::compile_and_run(R"(
        constexpr double_val := fn(constexpr x: i32): i32 {
            return x * 2;
        };
        pub const main := fn(): i32 {
            constexpr var arr: [3]i32 = .{10, 20, 30};
            arr[1] = 50;
            constexpr doubled := double_val(arr[1]);
            return doubled;
        };
    )") == 100);
}

TEST_CASE("mutated `constexpr var` struct field visible to nested `constexpr` call") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        constexpr add_pts := fn(constexpr a: i32, constexpr b: i32): i32 {
            return a + b;
        };
        pub const main := fn(): i32 {
            constexpr var p := Point{ .x = 10, .y = 20 };
            p.x += 5;
            constexpr sum := add_pts(p.x, p.y);
            return sum;
        };
    )") == 35);
}

TEST_CASE("`for constexpr` iterating over string slice in e2e execution") {
    CHECK(helpers::compile_and_run(R"(
        constexpr parse_usize := fn(constexpr digits: []u8): usize {
            constexpr var n: usize = 0;
            for constexpr (digits) |c| {
                n = n * 10 + @intCast(usize, c - '0');
            }
            return n;
        };
        pub const main := fn(): i32 {
            constexpr val := parse_usize("42");
            return @intCast(i32, val);
        };
    )") == 42);
}

TEST_CASE("constexpr call reaching unreachable fails compilation") {
    helpers::expect_compile_error(R"(
        constexpr bad := fn(): i32 {
            unreachable;
        };
        pub const main := fn(): i32 {
            constexpr x := bad();
            return x;
        };
    )");
}

} // namespace ghoti::tests
