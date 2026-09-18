#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`for constexpr` unrolls a range/array/pack driver without error") {
    helpers::resolve_and_check(R"(
        const use := fn(): void {
            var sum := 0;
            for constexpr (0..3) |v| { sum = sum + v; }
        };
    )");
    helpers::resolve_and_check(R"(
        constexpr arr: [3]i32 = .{1, 2, 3};
        const use := fn(): void {
            var sum := 0;
            for constexpr (arr) |v| { sum = sum + v; }
        };
    )");
    helpers::resolve_and_check(R"(
        const f := fn(rest...): void {
            var sum := 0;
            for constexpr (rest) |e| { sum = sum + e; }
        };
        const use := fn(): void { f(1, 2, 3); };
    )");
}

TEST_CASE("`for constexpr`'s trip count must be known at compile time") {
    CHECK(helpers::raised(R"(
        const use := fn(a: usize): void {
            for constexpr (0..a) |v| { _ = v; }
        };
    )",
                          sema::error::CONSTEXPR_LOOP_COUNT_NOT_STATIC));
    CHECK(helpers::raised(R"(
        const use := fn(a: [3]i32): void {
            for constexpr (a) |v| { _ = v; }
        };
    )",
                          sema::error::CONSTEXPR_LOOP_COUNT_NOT_STATIC));
    CHECK(helpers::raised(R"(
        const use := fn(): void {
            for constexpr (0..) |v| { _ = v; }
        };
    )",
                          sema::error::CONSTEXPR_LOOP_COUNT_NOT_STATIC));
}

TEST_CASE("`for constexpr` rejects mismatched-length drivers and a non-trailing open range") {
    CHECK(helpers::raised(R"(
        const use := fn(): void {
            constexpr a: [2]i32 = .{1, 2};
            constexpr b: [3]i32 = .{3, 4, 5};
            for constexpr (a, b) |x, y| { _ = x; _ = y; }
        };
    )",
                          sema::error::CONSTEXPR_LOOP_COUNT_NOT_STATIC));

    CHECK(helpers::raised(R"(
        const use := fn(): void {
            constexpr a: [2]i32 = .{1, 2};
            for constexpr (0.., a) |i, x| { _ = i; _ = x; }
        };
    )",
                          sema::error::CONSTEXPR_LOOP_COUNT_NOT_STATIC));
}

TEST_CASE("`for constexpr` rejects a `break`/`continue` at its own iteration boundary") {
    CHECK(helpers::raised(R"(
        const use := fn(): void {
            for constexpr (0..3) |v| { if (v == 1) { break; } }
        };
    )",
                          sema::error::CONSTEXPR_LOOP_BREAK));
    CHECK(helpers::raised(R"(
        const use := fn(): void {
            for constexpr (0..3) |v| { if (v == 1) { continue; } }
        };
    )",
                          sema::error::CONSTEXPR_LOOP_CONTINUE));
}

TEST_CASE("`for constexpr`'s break/continue restriction does not reach a nested ordinary loop") {
    helpers::resolve_and_check(R"(
        const use := fn(): void {
            for constexpr (0..3) |v| {
                for (0..v) |j| { if (j == 0) { break; } }
            }
        };
    )");
}

TEST_CASE("`while constexpr` rejects runtime `break`/`continue`") {
    CHECK(helpers::raised(R"(
        const use := fn(): void {
            var n := 0;
            while constexpr (true) { if (n == 1) { break; } n = n + 1; }
        };
    )",
                          sema::error::CONSTEXPR_LOOP_BREAK));
    CHECK(helpers::raised(R"(
        const use := fn(): void {
            var n := 0;
            while constexpr (true) { if (n == 1) { continue; } n = n + 1; }
        };
    )",
                          sema::error::CONSTEXPR_LOOP_CONTINUE));
}

TEST_CASE("`while constexpr` allows compile-time `break` and `continue`") {
    helpers::resolve_and_check(R"(
        const use := fn(): void {
            constexpr var n := 0;
            while constexpr (n < 5) {
                if constexpr (n == 2) {
                    n = n + 2;
                    continue;
                }
                if constexpr (n >= 4) {
                    break;
                }
                n = n + 1;
            }
        };
    )");
}

TEST_CASE("`do ... while constexpr` rejects runtime jumps and allows compile-time jumps") {
    CHECK(helpers::raised(R"(
        const use := fn(): void {
            var n := 0;
            do { if (n == 1) { break; } } while constexpr (true);
        };
    )",
                          sema::error::CONSTEXPR_LOOP_BREAK));
    CHECK(helpers::raised(R"(
        const use := fn(): void {
            var n := 0;
            do { if (n == 1) { continue; } } while constexpr (true);
        };
    )",
                          sema::error::CONSTEXPR_LOOP_CONTINUE));

    helpers::resolve_and_check(R"(
        const use := fn(): void {
            constexpr var n := 0;
            do {
                if constexpr (n == 1) { break; }
                n = n + 1;
            } while constexpr (n < 5);
        };
    )");
}

TEST_CASE("`loop constexpr` rejects runtime jumps and allows compile-time jumps") {
    CHECK(helpers::raised(R"(
        const use := fn(): void {
            var n := 0;
            loop constexpr { if (n == 1) { break; } }
        };
    )",
                          sema::error::CONSTEXPR_LOOP_BREAK));
    CHECK(helpers::raised(R"(
        const use := fn(): void {
            var n := 0;
            loop constexpr { if (n == 1) { continue; } }
        };
    )",
                          sema::error::CONSTEXPR_LOOP_CONTINUE));

    helpers::resolve_and_check(R"(
        const use := fn(): void {
            constexpr var n := 0;
            loop constexpr {
                if constexpr (n == 3) { break; }
                n = n + 1;
            }
        };
    )");
}

} // namespace ghoti::tests
