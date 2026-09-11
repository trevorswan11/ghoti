#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

// `while constexpr` still runs as an ordinary loop until unrolling lands (Part I §6 of the
// design; `for constexpr` unrolling landed in Phase 5). The `else`/labeled rejections are pure
// parse errors; see tests/ast/errors/test_pack_params.cc.
TEST_CASE("`while constexpr` parses and runs (not yet unrolled)") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var n := 0;
            while constexpr (n < 3) { n = n + 1; }
            return n;
        };
    )") == 3);
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

// `defer` fires at the end of its own iteration (same as inside an ordinary loop body), not
// accumulated to the enclosing scope; see the design doc's corrected §5.5.
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
