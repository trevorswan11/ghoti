#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("`and` binds tighter than `or`") {
    SECTION("at runtime") {
        CHECK(helpers::compile_and_run(R"(
            pub const main = fn(): i32 {
                let mut t = true;
                let mut f = false;
                // (t or f) and f would be false
                if (!(t or f and f)) { return 1; }
                // f and t or t is (f and t) or t either way
                if (!(f and t or t)) { return 2; }
                // t or f and f or f is t or (f and f) or f
                if (!(t or f and f or f)) { return 3; }
                return 0;
            };
        )") == 0);
    }

    SECTION("when folded") {
        CHECK(helpers::compile_and_run(R"(
            const A = true or false and false;
            const B = false and true or false and true;
            pub const main = fn(): i32 {
                if (!A) { return 1; }
                if (B) { return 2; }
                return 0;
            };
        )") == 0);
    }

    SECTION("in an `if comptime` condition") {
        CHECK(helpers::compile_and_run(R"(
            const pick = fn(comptime T: type): i32 {
                if comptime (T == bool or @typeInfo(T) == .int and @typeInfo(T).int.bits == 8) {
                    return 1;
                }
                return 0;
            };
            pub const main = fn(): i32 {
                return pick(u8) + pick(bool) * 10 + pick(u16) * 100;
            };
        )") == 11);
    }
}

} // namespace ghoti::tests
