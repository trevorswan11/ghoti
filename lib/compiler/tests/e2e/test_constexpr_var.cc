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

} // namespace ghoti::tests
