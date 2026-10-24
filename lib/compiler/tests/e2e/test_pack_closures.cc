#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("A capturing closure takes a parameter pack of any arity") {
    CHECK(helpers::compile_and_run(R"(
        const sum = fn(rest...): i32 {
            let mut t: i32 = 0;
            unroll for (rest) |v| { t += @as(i32, @intCast(v)); }
            return t;
        };
        pub const main = fn(): i32 {
            let bias: i32 = 1;
            let mut calls: i32 = 0;
            let total = fn(rest...): i32 {
                calls += 1;
                let mut t: i32 = 0;
                unroll for (rest) |v| { t += @as(i32, @intCast(v)) + bias; }
                return t;
            };
            let k: i32 = 10;
            let fwd = fn(rest...): i32 { return sum(rest...) * k; };
            let mixed = fn(a: auto, rest...): i32 {
                return @as(i32, @intCast(a)) + @as(i32, @intCast(rest.len)) + bias;
            };
            return total(1, 2, 3) + total(@as(u8, 4)) + total() + calls + fwd(1, 2) +
                   fwd(@as(i64, 3)) + mixed(@as(u8, 5), 1, 2) + mixed(@as(i64, 1));
        };
    )") == 9 + 5 + 0 + 3 + 30 + 30 + 8 + 2);
}

TEST_CASE("A closure reads and forwards the pack of the function around it") {
    CHECK(helpers::compile_and_run(R"(
        const outer = fn(rest...): i32 {
            let mut t: i32 = 0;
            let len = fn(): i32 { return @intCast(rest.len); };
            let add = fn(): void { unroll for (rest) |v| { t += @as(i32, @intCast(v)); } };
            let first = fn(i: auto): i32 {
                return @as(i32, @intCast(rest[0])) + @as(i32, @intCast(i));
            };
            let count = fn(xs...): i32 { return @intCast(xs.len); };
            add();
            return t + len() + first(@as(u8, 1)) + count(rest...);
        };
        pub const main = fn(): i32 { return outer(5, 6, 7) + outer(@as(i64, 9)); };
    )") == (18 + 3 + 6 + 3) + (9 + 1 + 10 + 1));
}

TEST_CASE("A closure with its own pack also reads the enclosing function's pack") {
    CHECK(helpers::compile_and_run(R"(
        const outer = fn(rest...): i32 {
            let inner = fn(more...): i32 {
                let mut t: i32 = 0;
                unroll for (rest) |e| { t += @as(i32, @intCast(e)); }
                unroll for (more) |m| { t += @as(i32, @intCast(m)) * 10; }
                return t + @as(i32, @intCast(rest.len + more.len)) * 20;
            };
            return inner(@as(u8, 1)) + inner(@as(i64, 2), 3);
        };
        pub const main = fn(): i32 { return outer(1, 2); };
    )") == (3 + 10 + 60) + (3 + 50 + 80));
}

TEST_CASE("A capturing generic is called from each iteration of an `unroll for` over a pack") {
    CHECK(helpers::compile_and_run(R"(
        const fold = fn(base: i32, rest...): i32 {
            let mut t: i32 = base;
            let add = fn(x: auto): void { t += @as(i32, @intCast(x)); };
            unroll for (rest) |e| { add(e); }
            return t;
        };
        const Acc = fn(comptime T: type): type {
            return struct {
                v: T,
                pub const fold = fn(&self, rest...): T {
                    let mut t: T = self.v;
                    let add = fn(x: auto): void { t += @as(T, @intCast(x)); };
                    unroll for (rest) |e| { add(e); }
                    return t;
                };
            };
        };
        pub const main = fn(): i32 {
            let a: Acc(i32) = .{ .v = 1 };
            return fold(1, 2, 3, @as(u8, 4)) + a.fold(2, 3, @as(u8, 4));
        };
    )") == 10 + 10);
}

TEST_CASE("A pack closure is passed as a value or takes one element per expected parameter") {
    CHECK(helpers::compile_and_run(R"(
        const apply = fn(g: auto): i32 { return g(1, 2) + g(@as(u8, 3)); };
        const apply_two = fn(g: fn(a: i32, b: i32): i32, x: i32): i32 { return g(x, x); };
        const apply_bound = fn(g: impl Fn(a: i32, b: i32): i32): i32 { return g(3, 4); };
        pub const main = fn(): i32 {
            let off: i32 = 10;
            let count = fn(rest...): i32 { return @as(i32, @intCast(rest.len)) + off; };
            let sum = fn(rest...): i32 {
                let mut t: i32 = off;
                unroll for (rest) |v| { t += v; }
                return t;
            };
            let one: fn(a: i32): i32 = sum;
            return apply(count) + apply_two(sum, 2) + one(5) + apply_bound(sum);
        };
    )") == 23 + 14 + 15 + 17);
}

TEST_CASE("A pack closure is returned, and runs at compile time") {
    CHECK(helpers::compile_and_run(R"(
        const make = fn(off: i32): auto {
            return move fn(rest...): i32 {
                let mut t: i32 = off;
                unroll for (rest) |v| { t += @as(i32, @intCast(v)); }
                return t;
            };
        };
        const keep = fn(rest...): auto {
            return move fn(): i32 {
                let mut t: i32 = 0;
                unroll for (rest) |e| { t += @as(i32, @intCast(e)); }
                return t;
            };
        };
        const sum_all = fn(rest...): i32 {
            let mut t: i32 = 0;
            let add = fn(x: i32): void { t += x; };
            unroll for (rest) |e| { add(e); }
            return t;
        };
        const folded = sum_all(1, 2, 3);
        const run = fn(): i32 {
            comptime {
                let k: i32 = 2;
                let twice = fn(rest...): i32 {
                    let mut t: i32 = 0;
                    unroll for (rest) |v| { t += v * k; }
                    return t;
                };
                @assert(twice(1, 2, 3) == 12);
                @assert(folded == 6);
            }
            return 1;
        };
        pub const main = fn(): i32 {
            let f = make(100);
            let g = keep(1, 2, @as(u8, 3));
            return f(1, 2) + f(@as(u8, 3)) - 200 + g() + folded + run();
        };
    )") == 6 + 6 + 6 + 1);
}

TEST_CASE("A `const` closure can't capture an element of the enclosing pack") {
    CHECK(helpers::raised(R"(
        const outer = fn(rest...): i32 {
            const bad = fn(): i32 { return rest[0]; };
            return bad();
        };
        pub const main = fn(): i32 { return outer(1); };
    )",
                          sema::error::ILLEGAL_BINDING_KIND));
}

} // namespace ghoti::tests
