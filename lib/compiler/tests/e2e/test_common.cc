#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("Pure noop function with loops") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): void {
            const a := [_]i32{};
            for (a) |_| {}
            while (false) {}
            do {} while (false);
            loop { break; }
        };
    )") == 0);
}

TEST_CASE("compound assignment operators use their own operator, not +") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 20;
            x -= 4;                 // 16
            x *= 3;                 // 48
            x /= 2;                 // 24
            x %= 10;                // 4
            x <<= 3;                // 32
            x >>= 1;                // 16
            x &= 12;                // 16 & 12 == 0
            x |= 1;                 // 1
            x ^= 6;                 // 1 ^ 6 == 7
            return x;
        };
    )") == 7);
}

TEST_CASE("@sizeOf / @bitSizeOf of a function-local type folds") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const Pair := struct { a: i32, b: i32 };
            const Small := packed struct { x: u3, y: u5 };
            if (@sizeOf(Pair) != 8) { return 1; }
            if (@sizeOf(Small) != 1 or @bitSizeOf(Small) != 8) { return 2; }
            var p: Pair = .{ .a = 3, .b = 4 };
            return p.a + p.b + @as(i32, @sizeOf(Pair));
        };
    )") == 15);
}

TEST_CASE("Forward references and mutual recursion resolve") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 { return pick(is_even(10)); };
        const pick := fn(b: bool): i32 { if (b) { return 7; } return 0; };
        const is_even := fn(n: i32): bool {
            if (n == 0) { return true; }
            return is_odd(n - 1);
        };
        const is_odd := fn(n: i32): bool {
            if (n == 0) { return false; }
            return is_even(n - 1);
        };
    )") == 7);
}

} // namespace ghoti::tests
