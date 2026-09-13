#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("`+=` implicitly widens a narrower float RHS to the LHS's type") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var accumulator: f64 = 0;
            const x: f32 = 12.0;
            accumulator += x;
            accumulator += 0.34f32;
            return @as(i32, accumulator * 100.0) - 1200;
        };
    )") == 34);
}

TEST_CASE("`+=` implicitly widens a narrower integer RHS to the LHS's type") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var total: i64 = 0;
            const x: i32 = 40;
            total += x;
            const y: u8 = 2;
            total += y;
            return @intCast(i32, total);
        };
    )") == 42);
}

} // namespace ghoti::tests
