#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`unroll for` unrolls a range/array/pack driver without error") {
    helpers::resolve_and_check(R"(
        const use = fn(): void {
            let mut sum = 0;
            unroll for (0..3) |v| { sum = sum + v; }
        };
    )");
    helpers::resolve_and_check(R"(
        const arr: [3]i32 = .{1, 2, 3};
        const use = fn(): void {
            let mut sum = 0;
            unroll for (arr) |v| { sum = sum + v; }
        };
    )");
    helpers::resolve_and_check(R"(
        const f = fn(rest...): void {
            let mut sum = 0;
            unroll for (rest) |e| { sum = sum + e; }
        };
        const use = fn(): void { f(1, 2, 3); };
    )");
}

TEST_CASE("`unroll for`'s trip count must be known at compile time") {
    CHECK(helpers::raised(R"(
        let use = fn(a: usize): void {
            unroll for (0..a) |v| { _ = v; }
        };
    )",
                          sema::error::UNROLLED_LOOP_COUNT_NOT_STATIC));
    CHECK(helpers::raised(R"(
        let use = fn(a: [3]i32): void {
            unroll for (a) |v| { _ = v; }
        };
    )",
                          sema::error::UNROLLED_LOOP_COUNT_NOT_STATIC));
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            unroll for (0..) |v| { _ = v; }
        };
    )",
                          sema::error::UNROLLED_LOOP_COUNT_NOT_STATIC));
}

TEST_CASE("`unroll for` rejects mismatched-length drivers and a non-trailing open range") {
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            const a: [2]i32 = .{1, 2};
            const b: [3]i32 = .{3, 4, 5};
            unroll for (a, b) |x, y| { _ = x; _ = y; }
        };
    )",
                          sema::error::UNROLLED_LOOP_COUNT_NOT_STATIC));

    CHECK(helpers::raised(R"(
        const use = fn(): void {
            const a: [2]i32 = .{1, 2};
            unroll for (0.., a) |i, x| { _ = i; _ = x; }
        };
    )",
                          sema::error::UNROLLED_LOOP_COUNT_NOT_STATIC));
}

TEST_CASE("`unroll for` rejects a `break`/`continue` at its own iteration boundary") {
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            unroll for (0..3) |v| { if (v == 1) { break; } }
        };
    )",
                          sema::error::UNROLLED_LOOP_BREAK));
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            unroll for (0..3) |v| { if (v == 1) { continue; } }
        };
    )",
                          sema::error::UNROLLED_LOOP_CONTINUE));
}

TEST_CASE("`unroll for`'s break/continue restriction does not reach a nested ordinary loop") {
    helpers::resolve_and_check(R"(
        const use = fn(): void {
            unroll for (0..3) |v| {
                for (0..v) |j| { if (j == 0) { break; } }
            }
        };
    )");
}

TEST_CASE("`unroll while` rejects runtime `break`/`continue`") {
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            let mut n = 0;
            unroll while (true) { if (n == 1) { break; } n = n + 1; }
        };
    )",
                          sema::error::UNROLLED_LOOP_BREAK));
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            let mut n = 0;
            unroll while (true) { if (n == 1) { continue; } n = n + 1; }
        };
    )",
                          sema::error::UNROLLED_LOOP_CONTINUE));
}

TEST_CASE("`unroll while` allows compile-time `break` and `continue`") {
    helpers::resolve_and_check(R"(
        const use = fn(): void {
            comptime let mut n = 0;
            unroll while (n < 5) {
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

TEST_CASE("`unroll do ... while` rejects runtime jumps and allows compile-time jumps") {
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            let mut n = 0;
            unroll do { if (n == 1) { break; } } while (true);
        };
    )",
                          sema::error::UNROLLED_LOOP_BREAK));
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            let mut n = 0;
            unroll do { if (n == 1) { continue; } } while (true);
        };
    )",
                          sema::error::UNROLLED_LOOP_CONTINUE));

    helpers::resolve_and_check(R"(
        const use = fn(): void {
            comptime let mut n = 0;
            unroll do {
                if comptime (n == 1) { break; }
                n = n + 1;
            } while (n < 5);
        };
    )");
}

TEST_CASE("`unroll loop` rejects runtime jumps and allows compile-time jumps") {
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            let mut n = 0;
            unroll loop { if (n == 1) { break; } }
        };
    )",
                          sema::error::UNROLLED_LOOP_BREAK));
    CHECK(helpers::raised(R"(
        const use = fn(): void {
            let mut n = 0;
            unroll loop { if (n == 1) { continue; } }
        };
    )",
                          sema::error::UNROLLED_LOOP_CONTINUE));

    helpers::resolve_and_check(R"(
        const use = fn(): void {
            comptime let mut n = 0;
            unroll loop {
                if comptime (n == 3) { break; }
                n = n + 1;
            }
        };
    )");
}

} // namespace ghoti::tests
