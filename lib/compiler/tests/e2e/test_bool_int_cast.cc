#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("@boolFromInt and @intFromBool roundtrip at runtime") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            let b_true := @boolFromInt(42);
            let b_false := @boolFromInt(0);
            if (!b_true or b_false) { return 1; }

            let i_one: i32 = @intFromBool(b_true);
            let i_zero: i32 = @intFromBool(b_false);
            if (i_one != 1 or i_zero != 0) { return 2; }

            let u_one := @intFromBool(u64, true);
            if (u_one != 1u64) { return 3; }

            return 0;
        };
    )") == 0);
}

TEST_CASE("@boolFromInt and @intFromBool const-evaluate at compile time") {
    CHECK(helpers::compile_and_run(R"(
        const B: bool = @boolFromInt(100);
        const N: i32 = @intFromBool(i32, B);
        pub const main := fn(): i32 {
            return if (B and N == 1) 0 else 1;
        };
    )") == 0);
}

} // namespace ghoti::tests
