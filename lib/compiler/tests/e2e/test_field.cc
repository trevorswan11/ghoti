#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`@field` reads a struct's own field by a compile-time name") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            const p := Point{ .x = 7, .y = 8 };
            return @field(p, "x") + @field(p, "y");
        };
    )") == 15);
}

TEST_CASE("`@field` reads a union's own field by a compile-time name") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { a: i32, b: i32 };
        pub const main := fn(): i32 {
            const u := U{ .a = 42 };
            return @field(u, "a");
        };
    )") == 42);
}

TEST_CASE("`@field` reads through a pointer and a reference") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        const via_ptr := fn(p: ^Point): i32 { return @field(p, "x"); };
        const via_ref := fn(p: &Point): i32 { return @field(p, "y"); };
        pub const main := fn(): i32 {
            var p := Point{ .x = 3, .y = 4 };
            return via_ptr(^p) + via_ref(&p);
        };
    )") == 7);
}

TEST_CASE(
    "`@field`'s name argument may be any compile-time-foldable expression, not just a literal") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        const field_name := fn(): []u8 { return "y"; };
        pub const main := fn(): i32 {
            const p := Point{ .x = 1, .y = 99 };
            return @field(p, field_name());
        };
    )") == 99);
}

TEST_CASE("`@field` is lvalue-capable: writing through it mutates the original") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            var p := Point{ .x = 1, .y = 2 };
            @field(p, "x") = 10;
            return p.x;
        };
    )") == 10);
}

TEST_CASE("`@field` rejects an unknown field name") {
    helpers::expect_compile_error(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            const p := Point{ .x = 1, .y = 2 };
            return @field(p, "z");
        };
    )");
}

TEST_CASE("`@field` on a type (not an instance) is a clean, explicit not-yet-implemented error") {
    helpers::expect_compile_error(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            return @field(Point, "x");
        };
    )");
}

} // namespace ghoti::tests
