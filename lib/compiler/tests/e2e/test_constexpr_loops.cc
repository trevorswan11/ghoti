#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`while constexpr` unrolls while its `constexpr var` condition holds") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var n := 0;
            while constexpr (n < 3) { n = n + 1; }
            return n;
        };
    )") == 3);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var n := 0;
            constexpr var sum := 0;
            while constexpr (n < 4) { sum = sum + n; n = n + 1; }
            return sum;
        };
    )") == 6);
}

TEST_CASE("`while constexpr` rejects a condition that can't fold to a compile-time `bool`") {
    helpers::expect_compile_error(R"(
        pub const main := fn(a: i32): i32 {
            while constexpr (a < 3) { a = a + 1; }
            return a;
        };
    )");
}

TEST_CASE("`while constexpr` stops at `@setEvalUnrollLimit` rather than looping forever") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            @setEvalUnrollLimit(5);
            constexpr var n := 0;
            while constexpr (n < 100) { n = n + 1; }
            return n;
        };
    )");
}

TEST_CASE("`for constexpr` unrolls over a compile-time range") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var sum := 0;
            for constexpr (0..3) |v| { sum = sum + v; }
            return sum;
        };
    )") == 3);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var sum := 0;
            for constexpr (1..=3) |v| { sum = sum + v; }
            return sum;
        };
    )") == 6);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var count := 0;
            for constexpr (0..0) |v| { count = count + 1; }
            return count;
        };
    )") == 0);
}

TEST_CASE("`for constexpr` unrolls over a module-level `constexpr` array") {
    CHECK(helpers::compile_and_run(R"(
        constexpr arr: [3]i32 = .{10, 20, 30};
        pub const main := fn(): i32 {
            var sum := 0;
            for constexpr (arr) |v| { sum = sum + v; }
            return sum;
        };
    )") == 60);
}

// Known limitation (§5.2): a local (function-scope) `constexpr` array's value isn't nameable to
// `for constexpr`'s own iterable-folding check, which runs during the resolve pass - only a
// module-level array's value is reachable there today. See the sibling known-limitation test in
// test_concat.cc for why a full fix (resolve-time AND emit-time registration, plus a
// materialize_const mismatch this surfaced) is bigger than this phase scoped.
TEST_CASE("`for constexpr` over a function-local `constexpr` array is a clean compile error") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            constexpr arr: [3]i32 = .{10, 20, 30};
            var sum := 0;
            for constexpr (arr) |v| { sum = sum + v; }
            return sum;
        };
    )");
}

TEST_CASE("`for constexpr` unrolls over a range with a companion `0..` index") {
    CHECK(helpers::compile_and_run(R"(
        constexpr arr: [3]i32 = .{10, 20, 30};
        pub const main := fn(): i32 {
            var sum := 0;
            for constexpr (arr, 0..) |v, i| { sum = sum + v + @intCast(i32, i); }
            return sum;
        };
    )") == 63);
}

// Two (or more) driving iterables in parallel - v1 was limited to one driver plus an optional
// companion `0..` index range; now any number of packs/ranges/`constexpr` array-or-slice values
// may drive the loop together, zipped by index like Zig's `for (a, b) |x, y|`.
TEST_CASE("`for constexpr` unrolls over two arrays in parallel") {
    CHECK(helpers::compile_and_run(R"(
        constexpr a: [3]i32 = .{1, 2, 3};
        constexpr b: [3]i32 = .{4, 5, 6};
        pub const main := fn(): i32 {
            var sum := 0;
            for constexpr (a, b) |x, y| { sum = sum + x * y; }
            return sum;
        };
    )") == 1 * 4 + 2 * 5 + 3 * 6);
}

TEST_CASE("`for constexpr` unrolls over three iterables (array, range, array) plus a companion "
         "index") {
    CHECK(helpers::compile_and_run(R"(
        constexpr a: [3]i32 = .{1, 2, 3};
        constexpr b: [3]i32 = .{4, 5, 6};
        pub const main := fn(): i32 {
            var sum := 0;
            for constexpr (a, 0..3, b, 0..) |x, r, y, i| {
                sum = sum + x + r + y + @intCast(i32, i);
            }
            return sum;
        };
    )") == (1 + 0 + 4 + 0) + (2 + 1 + 5 + 1) + (3 + 2 + 6 + 2));
}

// `weights` must be module-scope here, not a local inside `f`: a local `constexpr` array's own
// value isn't nameable to const-eval yet (a separate, pre-existing, documented limitation - see
// test_concat.cc - unrelated to parallel iteration itself).
TEST_CASE("`for constexpr` mixes a parameter pack with an array driver in parallel") {
    CHECK(helpers::compile_and_run(R"(
        constexpr weights: [3]i32 = .{2, 3, 5};
        const f := fn(rest...): i32 {
            var sum := 0;
            for constexpr (rest, weights) |e, w| { sum = sum + e * w; }
            return sum;
        };
        pub const main := fn(): i32 { return f(1, 2, 3); };
    )") == 1 * 2 + 2 * 3 + 3 * 5);
}

TEST_CASE("`for constexpr`'s parallel iterables must all have the same length") {
    helpers::expect_compile_error(R"(
        constexpr a: [2]i32 = .{1, 2};
        constexpr b: [3]i32 = .{1, 2, 3};
        pub const main := fn(): i32 {
            var sum := 0;
            for constexpr (a, b) |x, y| { sum = sum + x + y; }
            return sum;
        };
    )");
}

TEST_CASE("`for constexpr` unrolls over a parameter pack") {
    CHECK(helpers::compile_and_run(R"(
        const f := fn(rest...): i32 {
            var sum := 0;
            for constexpr (rest) |e| { sum = sum + e; }
            return sum;
        };
        pub const main := fn(): i32 { return f(1, 2, 3, 4); };
    )") == 10);
}

TEST_CASE("`for constexpr`'s own `break`/`continue` restriction does not reach a nested ordinary "
          "loop") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var count := 0;
            for constexpr (0..3) |v| {
                for (0..v) |j| { if (j == 0) { break; } count = count + 1; }
            }
            return count;
        };
    )") == 0);
}

TEST_CASE("`for constexpr`'s `defer` fires at the end of each iteration") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var order := 0;
            for constexpr (0..3) |v| { defer order = order * 10 + v; }
            return order;
        };
    )") == 12);
}

} // namespace ghoti::tests
