#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("A catch-all arm captures the scrutinee") {
    CHECK(helpers::compile_and_run(R"(
        const f = fn(v: i32): i32 {
            return match (v) { 0 => 100, 1..5 => 200, _ => |n| n * 2 };
        };
        pub const main = fn(): i32 { return if (f(0) + f(3) + f(21) == 342) 0 else 1; };
    )") == 0);

    CHECK(helpers::compile_and_run(R"(
        const E = enum { x, y, z };
        const h = fn(e: E): i32 {
            return match (e) { .x => 1, _ => |o| if (o == .y) 2 else 3 };
        };
        pub const main = fn(): i32 { return h(.x) * 100 + h(.y) * 10 + h(.z); };
    )") == 123);
}

TEST_CASE("A catch-all arm on a tagged union captures the whole union") {
    CHECK(helpers::compile_and_run(R"(
        const U = union(enum) { a: i32, b: u8, c: bool };
        const g = fn(u: U): i32 {
            return match (u) {
                .a => |n| n,
                _ => |rest| match (rest) { .b => |b| @intCast(i32, b) + 1000, .c => 7, .a => 0 },
            };
        };
        pub const main = fn(): i32 {
            return if (g(.{ .a = 5 }) == 5 and g(.{ .b = 2 }) == 1002 and g(.{ .c = true }) == 7)
                0 else 1;
        };
    )") == 0);
}

TEST_CASE("A catch-all capture can alias the scrutinee") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut x: i32 = 9;
            match (x) { 0 => {}, _ => |&mut p| { p += 1; } }
            return x;
        };
    )") == 10);
}

TEST_CASE("A catch-all capture folds at compile time") {
    CHECK(helpers::compile_and_run(R"(
        const k = comptime blk: {
            break :blk match comptime (@as(i32, 41)) { 0 => 0, _ => |n| n + 1 };
        };
        const triple = fn(comptime n: comptime_int): comptime_int {
            return match (n) { 0 => 0, _ => |v| v * 3 };
        };
        pub const main = fn(): i32 { return k + triple(2); };
    )") == 48);
}

} // namespace ghoti::tests
