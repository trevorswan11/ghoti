#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("A sentineled array of struct elements zero-fills its terminator") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            var pts: [2:0]Point = .{
                .{ .x = 1, .y = 2 },
                .{ .x = 3, .y = 4 },
            };
            if (pts[2].x != 0) { return 1; }
            if (pts[2].y != 0) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("A sentineled array of pointer elements terminates with null") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: i32 = 1;
            var b: i32 = 2;
            var ptrs: [2:0]^i32 = .{ ^a, ^b };
            if (ptrs[2] != nullptr) { return 1; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("`++` of struct-typed arrays materializes a zeroed sentinel byte") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        const a: [1]Point = .{ .{ .x = 1, .y = 2 } };
        const b: [1:0]Point = .{ .{ .x = 3, .y = 4 } };
        pub const main := fn(): i32 {
            var combined := a ++ b;
            if (combined[0].x != 1) { return 1; }
            if (combined[1].x != 3) { return 2; }
            if (combined[2].x != 0) { return 3; }
            return 0;
        };
    )") == 0);
}

} // namespace ghoti::tests
