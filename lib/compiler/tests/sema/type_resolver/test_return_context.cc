#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`return` supplies the implicit type for a bare initializer") {
    helpers::resolve_and_check(R"(
        const Point = struct { x: i32, y: i32 };
        const make = fn(): Point { return .{ .x = 1, .y = 2 }; };
    )");
}

TEST_CASE("`return` implicit-init context flows through an `if` expression") {
    helpers::resolve_and_check(R"(
        const Point = struct { x: i32, y: i32 };
        const pick = fn(b: bool): Point {
            return if (b) .{ .x = 1, .y = 2 }; else .{ .x = 3, .y = 4 };
        };
    )");
}

TEST_CASE("`return` implicit-init context flows through `match` arms") {
    helpers::resolve_and_check(R"(
        const Tag = enum { a, b };
        const Point = struct { x: i32, y: i32 };
        const from_tag = fn(t: Tag): Point {
            return match (t) {
                .a => .{ .x = 1, .y = 2 },
                .b => .{ .x = 3, .y = 4 },
            };
        };
    )");
}

TEST_CASE("`return` implicit-init context flows through a labeled loop `break`") {
    helpers::resolve_and_check(R"(
        const Point = struct { x: i32, y: i32 };
        const build = fn(): Point {
            let mut i: i32 = 0;
            return outer: loop {
                i += 1;
                if (i == 2) { break :outer .{ .x = 1, .y = 2 }; }
            };
        };
    )");
}

TEST_CASE("`return` supplies the implicit type for a bare `.variant`") {
    helpers::resolve_and_check(R"(
        const Tag = enum { a, b, c };
        const last = fn(): Tag { return .c; };
    )");
}

TEST_CASE("a `match` arm value is not forced to the matcher type") {
    helpers::test_resolver_fail(
        R"(
        const Src = enum { a, b };
        const Dst = enum { x, y };
        const f = fn(s: Src): i32 {
            let d = match (s) { .a => .x, .b => .y };
            return match (d) { .x => 0, .y => 1 };
        };
    )",
        // The arms are enum literals, which a runtime `let` can't hold
        sema::diagnostic{"an enum literal only exists at compile time, so it cannot be stored in "
                         "a 'let' or 'let mut' binding; give the binding an enum type (`let x: "
                         "E = .name;`) or use 'const' instead",
                         sema::error::MUTABLE_TYPE_BINDING,
                         std::pair{4UZ, 12UZ}});
}

} // namespace ghoti::tests
