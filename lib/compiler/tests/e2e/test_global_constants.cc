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

TEST_CASE("Top-level struct const with a nested array field") {
    CHECK(helpers::compile_and_run(R"(
        const Box := struct { items: [3]i32 };
        const B := Box{ .items = [_]i32{4, 5, 6} };
        pub const main := fn(): i32 {
            return B.items[2];
        };
    )") == 6);
}

} // namespace ghoti::tests
