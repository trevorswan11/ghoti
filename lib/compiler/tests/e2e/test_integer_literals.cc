#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("bare literal lower bound coerces to the upper bound's type in a range") {
    CHECK(helpers::compile_and_run(R"(
        const sum := fn(s: []i32): i32 {
            var total: i32 = 0;
            for (0..s.len, s) |i, _| {
                total += s[i];
            }
            return total;
        };

        pub const main := fn(): i32 {
            var a: [4uz]i32 = [4uz]i32{10, 11, 12, 9};
            return sum(a[0..4]);
        };
    )") == 42);
}

TEST_CASE("bare literal in a comparison coerces to a `usize` operand") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: [5uz]i32 = [5uz]i32{0, 0, 0, 0, 0};
            const s := a[0..5];
            var count: i32 = 0;
            for (0..s.len) |i| {
                if (i < s.len) { count += 1; }
                if (i >= 0) { count += 1; }
            }
            return count;  // 5 + 5
        };
    )") == 10);
}

TEST_CASE("bare literal upper bound coerces to the lower bound's type") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const lo: usize = 2uz;
            var hits: i32 = 0;
            for (lo..7) |_| { hits += 1; }
            return hits;  // 2,3,4,5,6
        };
    )") == 5);
}

} // namespace ghoti::tests
