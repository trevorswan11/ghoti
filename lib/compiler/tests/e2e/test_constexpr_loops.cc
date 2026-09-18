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

TEST_CASE("`for constexpr` unrolls over a function-local `constexpr` array") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr arr: [3]i32 = .{10, 20, 30};
            var sum := 0;
            for constexpr (arr) |v| { sum = sum + v; }
            return sum;
        };
    )") == 60);
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

TEST_CASE("`while constexpr` allows compile-time `continue` and `break`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var n := 0;
            constexpr var sum := 0;
            while constexpr (n < 5) : (n = n + 1) {
                if constexpr (n == 2) { continue; }
                sum = sum + n;
            }
            return sum;
        };
    )") == 8);

    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var n := 0;
            while constexpr (true) {
                if constexpr (n == 3) { break; }
                n = n + 1;
            }
            return n;
        };
    )") == 3);
}

TEST_CASE("`do ... while constexpr` unrolls and executes body at least once") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var n := 0;
            do {
                n = n + 1;
            } while constexpr (n < 4);
            return n;
        };
    )") == 4);

    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var n := 0;
            do {
                n = n + 1;
            } while constexpr (false);
            return n;
        };
    )") == 1);

    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var n := 0;
            do {
                if constexpr (n == 2) { break; }
                n = n + 1;
            } while constexpr (n < 10);
            return n;
        };
    )") == 2);
}

TEST_CASE("`loop constexpr` unrolls and breaks at compile time") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var n := 0;
            loop constexpr {
                if constexpr (n == 5) { break; }
                n = n + 1;
            }
            return n;
        };
    )") == 5);
}

TEST_CASE("`for constexpr` allows compile-time `continue` and `break`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var sum := 0;
            for constexpr (0..5) |v| {
                if constexpr (v == 2) { continue; }
                if constexpr (v == 4) { break; }
                sum = sum + v;
            }
            return sum;
        };
    )") == 0 + 1 + 3);
}

} // namespace ghoti::tests
