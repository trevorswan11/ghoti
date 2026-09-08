#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("Top-level array const indexed with a compile-time-constant index") {
    CHECK(helpers::compile_and_run(R"(
        const ARR := [3]i32{7, 8, 9};
        pub const main := fn(): i32 {
            return ARR[1];
        };
    )") == 8);
}

TEST_CASE("Top-level array const indexed with a runtime (non-constant) index") {
    CHECK(helpers::compile_and_run(R"(
        const ARR := [3]i32{7, 8, 9};
        pub const main := fn(): i32 {
            var i: usize = 2;
            return ARR[i];
        };
    )") == 9);
}

TEST_CASE("Top-level struct const field access") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        const P := Point{ .x = 7, .y = 8 };
        pub const main := fn(): i32 {
            return P.y;
        };
    )") == 8);
}

TEST_CASE("Top-level tagged union const active field access") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { flag: bool, val: i32 };
        const V := U{ .val = 42 };
        pub const main := fn(): i32 {
            return V.val;
        };
    )") == 42);
}

TEST_CASE("Top-level const initialized by a match with a capturing arm") {
    CHECK(helpers::compile_and_run(R"(
        const Payload := union { A: i32, B: i32 };
        const u := Payload{ .A = 5 };
        const y := match (u) {
            .A => |v| v,
            .B => 0,
        };
        pub const main := fn(): i32 {
            return y;
        };
    )") == 5);
}

TEST_CASE("Top-level struct const with a nested array field") {
    CHECK(helpers::compile_and_run(R"(
        const Box := struct { items: [3]i32 };
        const B := Box{ .items = [_]i32{4, 5, 6} };
        pub const main := fn(): i32 {
            return B.items[2];
        };
    )") == 6);
}

TEST_CASE("Top-level string const with an explicit slice type is usable, not a raw i8*") {
    CHECK(helpers::compile_and_run(R"(
        const S: []u8 = "abcd";
        pub const main := fn(): i32 {
            return @as(i32, @as(u32, S.len));
        };
    )") == 4);
}

TEST_CASE("Top-level sentinel-string const passed to a function as a slice") {
    CHECK(helpers::compile_and_run(R"(
        const NAME: [:0]u8 = "hijkl";
        const first := fn(s: []u8): u8 {
            return s[0];
        };
        pub const main := fn(): i32 {
            return @as(i32, first(NAME));
        };
    )") == 'h');
}

TEST_CASE("Top-level sentinel-string const iterated at runtime") {
    CHECK(helpers::compile_and_run(R"(
        const P: [:0]u8 = "ABC";
        pub const main := fn(): i32 {
            var n: i32 = 0;
            for (P) |c| { n += @as(i32, c); }
            return n - 100;
        };
    )") == ('A' + 'B' + 'C' - 100));
}

} // namespace ghoti::tests
