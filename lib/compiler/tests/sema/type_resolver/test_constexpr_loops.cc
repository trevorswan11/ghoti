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

TEST_CASE("`for constexpr` takes at most one driving iterable and one companion `0..` range") {
    CHECK(helpers::raised(R"(
        const use := fn(): void {
            constexpr a: [2]i32 = .{1, 2};
            constexpr b: [2]i32 = .{3, 4};
            for constexpr (a, b, 0..) |x, y, i| { _ = x; _ = y; _ = i; }
        };
    )",
                          sema::error::CONSTEXPR_LOOP_COUNT_NOT_STATIC));
    CHECK(helpers::raised(R"(
        const use := fn(): void {
            constexpr a: [2]i32 = .{1, 2};
            for constexpr (a, 1..3) |x, i| { _ = x; _ = i; }
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

} // namespace ghoti::tests
