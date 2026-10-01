#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("a function filling an `undefined` array in a runtime loop folds at compile time") {
    CHECK(helpers::compile_and_run(R"(
        const build := fn(comptime n: usize): [n]mut i32 {
            let mut out: [n]mut i32 = undefined;
            let mut i: usize = 0;
            while (i < n) { out[i] = @intCast(i32, i) * 3; i += 1; }
            return out;
        };
        const TABLE := build(4);
        pub const main := fn(): i32 {
            let small := comptime build(3);
            return TABLE[3] + small[2];
        };
    )") == 9 + 6);
}

TEST_CASE("an `undefined` struct is filled field by field at compile time") {
    CHECK(helpers::compile_and_run(R"(
        const P := struct { a: i32, b: i32 };
        const make := fn(): P {
            let mut out: P = undefined;
            out.a = 1;
            out.b = 2;
            return out;
        };
        const VALUE := make();
        pub const main := fn(): i32 { return VALUE.a + VALUE.b; };
    )") == 3);
}

TEST_CASE("nested `undefined` aggregates take their full shape") {
    CHECK(helpers::compile_and_run(R"(
        const Cell := struct { v: i32, row: [2]mut i32 };
        const make := fn(): [2]mut Cell {
            let mut grid: [2]mut Cell = undefined;
            let mut i: usize = 0;
            while (i < 2) {
                grid[i].v = @intCast(i32, i);
                grid[i].row[0] = 10;
                grid[i].row[1] = 20;
                i += 1;
            }
            return grid;
        };
        const GRID := make();
        pub const main := fn(): i32 { return GRID[1].v + GRID[0].row[1]; };
    )") == 1 + 20);
}

TEST_CASE("elements never written stay undefined after a compile-time fill") {
    CHECK(helpers::compile_and_run(R"(
        const half := fn(): [4]mut i32 {
            let mut out: [4]mut i32 = undefined;
            out[0] = 7;
            return out;
        };
        const HALF := half();
        pub const main := fn(): i32 { return HALF[0]; };
    )") == 7);
}

TEST_CASE("reading an element left `undefined` is a compile-time error") {
    helpers::expect_compile_error(R"(
        const bad := fn(): i32 {
            let mut out: [2]mut i32 = undefined;
            return out[1] + 1;
        };
        const VALUE := bad();
        pub const main := fn(): i32 { return VALUE; };
    )");
}

} // namespace ghoti::tests
