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
