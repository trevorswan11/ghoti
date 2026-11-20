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

TEST_CASE("shifts and bitwise operators follow C's precedence") {
    CHECK(helpers::compile_and_run(R"(
        const check = fn(u: u32, bits: u32): i32 {
            // u >> (bits - 1)
            if (u >> bits - 1 != 1) { return 1; }
            // (1 << 3) - 1 needs its parentheses, 1 << 3 - 1 is 1 << 2
            if ((1 << 3) - 1 != 7 or 1 << 3 - 1 != 4) { return 2; }
            // u & (bits + 1), not (u & bits) + 1
            if (u & bits + 1 != 0) { return 3; }
            // 1 | (6 & 3) and 1 | (2 ^ 3)
            if (1 | 6 & 3 != 3 or 1 | 2 ^ 3 != 1) { return 4; }
            // (u >> 31) & 1 binds the shift first
            if (u >> 31 & 1 != 1) { return 5; }
            // comparisons stay looser than `&`
            if (!(u & 1 == 0)) { return 6; }
            return 0;
        };
        const FOLDED = check(0x8000_0000, 32);
        pub const main = fn(): i32 {
            if (FOLDED != 0) { return 10 + FOLDED; }
            return check(0x8000_0000, 32);
        };
    )") == 0);
}

} // namespace ghoti::tests
