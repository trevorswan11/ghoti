#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`for comptime` unrolls a range/array/pack driver without error") {
    helpers::resolve_and_check(R"(
        const use = fn(): void {
            let mut sum = 0;
            for comptime (0..3) |v| { sum = sum + v; }
        };
    )");
    helpers::resolve_and_check(R"(
        const arr: [3]i32 = .{1, 2, 3};
        const use = fn(): void {
            let mut sum = 0;
            for comptime (arr) |v| { sum = sum + v; }
        };
    )");
    helpers::resolve_and_check(R"(
        const f = fn(rest...): void {
            let mut sum = 0;
            for comptime (rest) |e| { sum = sum + e; }
        };
        const use = fn(): void { f(1, 2, 3); };
    )");
}

TEST_CASE("`for comptime`'s trip count must be known at compile time") {
    CHECK(helpers::raised(R"(
        let use = fn(a: usize): void {
            for comptime (0..a) |v| { _ = v; }
        };
    )",
                          sema::error::COMPTIME_LOOP_COUNT_NOT_STATIC));
    CHECK(helpers::raised(R"(
        let use = fn(a: [3]i32): void {
            for comptime (a) |v| { _ = v; }
        };
    )",
                          sema::error::COMPTIME_LOOP_COUNT_NOT_STATIC));
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            for comptime (0..) |v| { _ = v; }
        };
    )",
                          sema::error::COMPTIME_LOOP_COUNT_NOT_STATIC));
}

TEST_CASE("`for comptime` rejects mismatched-length drivers and a non-trailing open range") {
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            const a: [2]i32 = .{1, 2};
            const b: [3]i32 = .{3, 4, 5};
            for comptime (a, b) |x, y| { _ = x; _ = y; }
        };
    )",
                          sema::error::COMPTIME_LOOP_COUNT_NOT_STATIC));

    CHECK(helpers::raised(R"(
        const use = fn(): void {
            const a: [2]i32 = .{1, 2};
            for comptime (0.., a) |i, x| { _ = i; _ = x; }
        };
    )",
                          sema::error::COMPTIME_LOOP_COUNT_NOT_STATIC));
}

TEST_CASE("`for comptime` rejects a `break`/`continue` at its own iteration boundary") {
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            for comptime (0..3) |v| { if (v == 1) { break; } }
        };
    )",
                          sema::error::COMPTIME_LOOP_BREAK));
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            for comptime (0..3) |v| { if (v == 1) { continue; } }
        };
    )",
                          sema::error::COMPTIME_LOOP_CONTINUE));
}

TEST_CASE("`for comptime`'s break/continue restriction does not reach a nested ordinary loop") {
    helpers::resolve_and_check(R"(
        const use = fn(): void {
            for comptime (0..3) |v| {
                for (0..v) |j| { if (j == 0) { break; } }
            }
        };
    )");
}

TEST_CASE("`while comptime` rejects runtime `break`/`continue`") {
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            let mut n = 0;
            while comptime (true) { if (n == 1) { break; } n = n + 1; }
        };
    )",
                          sema::error::COMPTIME_LOOP_BREAK));
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            let mut n = 0;
            while comptime (true) { if (n == 1) { continue; } n = n + 1; }
        };
    )",
                          sema::error::COMPTIME_LOOP_CONTINUE));
}

TEST_CASE("`while comptime` allows compile-time `break` and `continue`") {
    helpers::resolve_and_check(R"(
        const use = fn(): void {
            comptime let mut n = 0;
            while comptime (n < 5) {
                if comptime (n == 2) {
                    n = n + 2;
                    continue;
                }
                if comptime (n >= 4) {
                    break;
                }
                n = n + 1;
            }
        };
    )");
}

TEST_CASE("`do ... while comptime` rejects runtime jumps and allows compile-time jumps") {
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            let mut n = 0;
            do { if (n == 1) { break; } } while comptime (true);
        };
    )",
                          sema::error::COMPTIME_LOOP_BREAK));
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            let mut n = 0;
            do { if (n == 1) { continue; } } while comptime (true);
        };
    )",
                          sema::error::COMPTIME_LOOP_CONTINUE));

    helpers::resolve_and_check(R"(
        const use = fn(): void {
            comptime let mut n = 0;
            do {
                if comptime (n == 1) { break; }
                n = n + 1;
            } while comptime (n < 5);
        };
    )");
}

TEST_CASE("`loop comptime` rejects runtime jumps and allows compile-time jumps") {
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            let mut n = 0;
            loop comptime { if (n == 1) { break; } }
        };
    )",
                          sema::error::COMPTIME_LOOP_BREAK));
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            let mut n = 0;
            loop comptime { if (n == 1) { continue; } }
        };
    )",
                          sema::error::COMPTIME_LOOP_CONTINUE));

    helpers::resolve_and_check(R"(
        const use = fn(): void {
            comptime let mut n = 0;
            loop comptime {
                if comptime (n == 3) { break; }
                n = n + 1;
            }
        };
    )");
}

} // namespace ghoti::tests
