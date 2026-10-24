#include <string>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`unroll while` unrolls while its `comptime let mut` condition holds") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime let mut n = 0;
            unroll while (n < 3) { n = n + 1; }
            return n;
        };
    )") == 3);
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime let mut n = 0;
            comptime let mut sum = 0;
            unroll while (n < 4) { sum = sum + n; n = n + 1; }
            return sum;
        };
    )") == 6);
}

TEST_CASE("`unroll while` rejects a condition that can't fold to a compile-time `bool`") {
    helpers::expect_compile_error(R"(
        let step = fn(a: i32): i32 {
            unroll while (a < 3) { a = a + 1; }
            return a;
        };
        pub const main = fn(): i32 { return step(1); };
    )");
}

TEST_CASE("`unroll while` stops at `@setEvalUnrollLimit` rather than looping forever") {
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            @setEvalUnrollLimit(5);
            comptime let mut n = 0;
            unroll while (n < 100) { n = n + 1; }
            return n;
        };
    )");
}

TEST_CASE("`unroll for` unrolls over a compile-time range") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut sum = 0;
            unroll for (0..3) |v| { sum = sum + v; }
            return sum;
        };
    )") == 3);
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut sum = 0;
            unroll for (1..=3) |v| { sum = sum + v; }
            return sum;
        };
    )") == 6);
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut count = 0;
            unroll for (0..0) |v| { count = count + 1; }
            return count;
        };
    )") == 0);
}

TEST_CASE("`unroll for` unrolls over a module-level `comptime` array") {
    CHECK(helpers::compile_and_run(R"(
        const arr: [3]i32 = .{10, 20, 30};
        pub const main = fn(): i32 {
            let mut sum = 0;
            unroll for (arr) |v| { sum = sum + v; }
            return sum;
        };
    )") == 60);
}

TEST_CASE("`unroll for` unrolls over a function-local `comptime` array") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            const arr: [3]i32 = .{10, 20, 30};
            let mut sum = 0;
            unroll for (arr) |v| { sum = sum + v; }
            return sum;
        };
    )") == 60);
}

TEST_CASE("`unroll for` unrolls over a range with a companion `0..` index") {
    CHECK(helpers::compile_and_run(R"(
        const arr: [3]i32 = .{10, 20, 30};
        pub const main = fn(): i32 {
            let mut sum = 0;
            unroll for (arr, 0..) |v, i| { sum = sum + v + @intCast(i32, i); }
            return sum;
        };
    )") == 63);
}

TEST_CASE("`unroll for` unrolls over two arrays in parallel") {
    CHECK(helpers::compile_and_run(R"(
        const a: [3]i32 = .{1, 2, 3};
        const b: [3]i32 = .{4, 5, 6};
        pub const main = fn(): i32 {
            let mut sum = 0;
            unroll for (a, b) |x, y| { sum = sum + x * y; }
            return sum;
        };
    )") == 1 * 4 + 2 * 5 + 3 * 6);
}

TEST_CASE("`unroll for` unrolls over three iterables (array, range, array) plus a companion "
          "index") {
    CHECK(helpers::compile_and_run(R"(
        const a: [3]i32 = .{1, 2, 3};
        const b: [3]i32 = .{4, 5, 6};
        pub const main = fn(): i32 {
            let mut sum = 0;
            unroll for (a, 0..3, b, 0..) |x, r, y, i| {
                sum = sum + x + r + y + @intCast(i32, i);
            }
            return sum;
        };
    )") == (1 + 0 + 4 + 0) + (2 + 1 + 5 + 1) + (3 + 2 + 6 + 2));
}

TEST_CASE("`unroll for` mixes a parameter pack with an array driver in parallel") {
    CHECK(helpers::compile_and_run(R"(
        const weights: [3]i32 = .{2, 3, 5};
        const f = fn(rest...): i32 {
            let mut sum = 0;
            unroll for (rest, weights) |e, w| { sum = sum + e * w; }
            return sum;
        };
        pub const main = fn(): i32 { return f(1, 2, 3); };
    )") == 1 * 2 + 2 * 3 + 3 * 5);
}

TEST_CASE("`unroll for`'s parallel iterables must all have the same length") {
    helpers::expect_compile_error(R"(
        const a: [2]i32 = .{1, 2};
        const b: [3]i32 = .{1, 2, 3};
        pub const main = fn(): i32 {
            let mut sum = 0;
            unroll for (a, b) |x, y| { sum = sum + x + y; }
            return sum;
        };
    )");
}

TEST_CASE("`unroll for` unrolls over a parameter pack") {
    CHECK(helpers::compile_and_run(R"(
        const f = fn(rest...): i32 {
            let mut sum = 0;
            unroll for (rest) |e| { sum = sum + e; }
            return sum;
        };
        pub const main = fn(): i32 { return f(1, 2, 3, 4); };
    )") == 10);
}

TEST_CASE("`unroll for`'s own `break`/`continue` restriction does not reach a nested ordinary "
          "loop") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut count = 0;
            unroll for (0..3) |v| {
                for (0..v) |j| { if (j == 0) { break; } count = count + 1; }
            }
            return count;
        };
    )") == 0);
}

TEST_CASE("`unroll for`'s `defer` fires at the end of each iteration") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut order = 0;
            unroll for (0..3) |v| { defer order = order * 10 + v; }
            return order;
        };
    )") == 12);
}

TEST_CASE("`unroll while` allows compile-time `continue` and `break`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime let mut n = 0;
            comptime let mut sum = 0;
            unroll while (n < 5) : (n = n + 1) {
                if comptime (n == 2) { continue; }
                sum = sum + n;
            }
            return sum;
        };
    )") == 8);

    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime let mut n = 0;
            unroll while (true) {
                if comptime (n == 3) { break; }
                n = n + 1;
            }
            return n;
        };
    )") == 3);
}

TEST_CASE("`unroll do ... while` unrolls and executes body at least once") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime let mut n = 0;
            unroll do {
                n = n + 1;
            } while (n < 4);
            return n;
        };
    )") == 4);

    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime let mut n = 0;
            unroll do {
                n = n + 1;
            } while (false);
            return n;
        };
    )") == 1);

    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime let mut n = 0;
            unroll do {
                if comptime (n == 2) { break; }
                n = n + 1;
            } while (n < 10);
            return n;
        };
    )") == 2);
}

TEST_CASE("`unroll loop` unrolls and breaks at compile time") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime let mut n = 0;
            unroll loop {
                if comptime (n == 5) { break; }
                n = n + 1;
            }
            return n;
        };
    )") == 5);
}

TEST_CASE("`unroll for` allows compile-time `continue` and `break`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut sum = 0;
            unroll for (0..5) |v| {
                if comptime (v == 2) { continue; }
                if comptime (v == 4) { break; }
                sum = sum + v;
            }
            return sum;
        };
    )") == 0 + 1 + 3);
}

TEST_CASE("a loop's `else` branch runs only when the loop finishes, at runtime and compile time") {
    constexpr auto loops{R"(
        const w = fn(n: i32, stop: i32): i32 {
            let mut c: i32 = 0;
            let mut i: i32 = 0;
            while (i < n) : (i += 1) { if (i == stop) { break; } c += 1; } else { c += 50; }
            return c;
        };
        const f = fn(n: usize, stop: usize): i32 {
            let mut c: i32 = 0;
            for (0..n) |i| { if (i == stop) { break; } c += 1; } else { c += 50; }
            return c;
        };
        const lw = fn(n: i32): i32 {
            let mut i: i32 = 0;
            return blk: while (i < n) : (i += 1) { if (i == 7) { break :blk 99; } } else 40;
        };
        const lf = fn(n: usize): i32 {
            return blk: for (0..n) |i| { if (i == 7) { break :blk 99; } } else 40;
        };
        const total = fn(): i32 {
            return w(4, 99) + w(4, 2) + f(4, 99) + f(4, 2) + lw(3) + lw(9) + lf(3) + lf(9);
        };
    )"};
    constexpr i32  expected{54 + 2 + 54 + 2 + 40 + 99 + 40 + 99};
    CHECK(helpers::compile_and_run(std::string{loops} +
                                   "pub const main = fn(): i32 { return total() - 300; };") ==
          expected - 300);
    CHECK(helpers::compile_and_run(
              std::string{loops} +
              "pub const main = fn(): i32 { const v = total(); return v - 300; };") ==
          expected - 300);
}

TEST_CASE("`continue` and labeled jumps fold like their runtime counterparts") {
    constexpr auto jumps{R"(
        const d = fn(): i32 {
            let mut i: i32 = 0;
            let mut s: i32 = 0;
            do { i += 1; if (i == 2) { continue; } s += i; } while (i < 5);
            return s;
        };
        const x = fn(): i32 {
            let mut s: i32 = 0;
            outer: for (0..3) |i| { for (0..3) |j| { if (j == 1) { continue :outer; } s += 1; } }
            return s;
        };
        const y = fn(): i32 {
            let mut s: i32 = 0;
            outer: while (s < 100) {
                let mut j: i32 = 0;
                while (j < 5) : (j += 1) { if (j == 2) { break :outer; } s += 1; }
            }
            return s;
        };
    )"};
    CHECK(helpers::compile_and_run(std::string{jumps} +
                                   "pub const main = fn(): i32 { return d() + x() + y(); };") ==
          18);
    CHECK(helpers::compile_and_run(
              std::string{jumps} +
              "pub const main = fn(): i32 { const v = d() + x() + y(); return v; };") == 18);
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime {
                let mut t: i32 = 0;
                blk: { t = 1; break :blk; }
                t += 10;
                @assert(t == 11);
            }
            return 0;
        };
    )") == 0);
}

TEST_CASE("unrolled loops in a generic fn retype every iteration per instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const by_for = fn(comptime T: type, x: T): T {
            const Wide = @Int(.{ .signedness = .unsigned, .bits = @typeInfo(T).int.bits * 2 });
            let mut y = x;
            unroll for (0..3) |_| {
                y = @truncate(@as(Wide, y) * 2);
            }
            return y;
        };

        const by_while = fn(comptime T: type, x: T): T {
            const Wide = @Int(.{ .signedness = .unsigned, .bits = @typeInfo(T).int.bits * 2 });
            let mut y = x;
            comptime let mut U: type = T;
            comptime let mut k = 0;
            unroll while (k < 3) {
                U = T;
                y = @truncate(@as(Wide, y) * 2);
                k = k + 1;
            }
            return y;
        };

        const by_do_while = fn(comptime T: type, x: T): T {
            const Wide = @Int(.{ .signedness = .unsigned, .bits = @typeInfo(T).int.bits * 2 });
            let mut y = x;
            comptime let mut k = 0;
            unroll do {
                y = @truncate(@as(Wide, y) * 2);
                k = k + 1;
            } while (k < 3);
            return y;
        };

        const by_loop = fn(comptime T: type, x: T): T {
            const Wide = @Int(.{ .signedness = .unsigned, .bits = @typeInfo(T).int.bits * 2 });
            let mut y = x;
            comptime let mut k = 0;
            unroll loop {
                if comptime (k == 3) { break; }
                y = @truncate(@as(Wide, y) * 2);
                k = k + 1;
            }
            return y;
        };

        pub const main = fn(): i32 {
            if (by_for(u32, 1) != 8 or by_for(u64, 1) != 8) return 1;
            if (by_while(u32, 1) != 8 or by_while(u64, 1) != 8) return 2;
            if (by_do_while(u32, 1) != 8 or by_do_while(u64, 1) != 8) return 3;
            if (by_loop(u32, 1) != 8 or by_loop(u64, 1) != 8) return 4;
            return 42;
        };
    )") == 42);
}

TEST_CASE("`comptime` before a loop evaluates it like a `comptime` block") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime { unroll for (1..=4) |v| { @assert(v > 0); } }
            comptime unroll for (1..=4) |v| { @assert(v > 0); };
            comptime for (0..3) |v| { @assert(v < 3); };
            comptime unroll loop { break; };
            return 7;
        };
    )") == 7);
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            comptime unroll for (0..3) |v| { @assert(v < 2); };
            return 0;
        };
    )");
}

TEST_CASE("`unroll for` over a range past the unroll limit is an error, not a hang") {
    CHECK(helpers::raised(R"(
        pub const f = fn(): void { unroll for (0..1000000) |i| { _ = i; } };
    )",
                          sema::error::UNROLLED_LOOP_LIMIT));
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            @setEvalUnrollLimit(1000);
            let mut sum: i32 = 0;
            unroll for (0..500) |i| { sum += @intCast(i % 2); }
            return sum;
        };
    )") == 250);
}

} // namespace ghoti::tests
