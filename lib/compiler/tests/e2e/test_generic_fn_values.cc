#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("A generic function is passed as a value to an `auto` parameter") {
    CHECK(helpers::compile_and_run(R"(
        const apply = fn(g: auto, v: i32): i32 { return g(v); };
        const twice = fn(x: auto): @TypeOf(x) { return x * 2; };

        pub const main = fn(): i32 {
            let off: i32 = 1;
            let plain = fn(x: auto): i32 { return @as(i32, @intCast(x)) + 1; };
            let capturing = fn(x: auto): i32 { return @as(i32, @intCast(x)) + off; };
            return apply(plain, 4) + apply(capturing, 5) + apply(twice, 7) +
                   apply(fn(x: auto): i32 { return @as(i32, @intCast(x)) - off; }, 9);
        };
    )") == 5 + 6 + 14 + 8);
}

TEST_CASE("An `auto` parameter holding a generic function calls it at several types") {
    CHECK(helpers::compile_and_run(R"(
        const both = fn(g: auto): i32 { return g(@as(u8, 1)) + g(@as(i64, 2)); };

        pub const main = fn(): i32 {
            let mut calls: i32 = 0;
            let g = fn(x: auto): i32 { calls += 1; return @as(i32, @intCast(x)) * 10; };
            return both(g) + calls;
        };
    )") == 30 + 2);
}

TEST_CASE("A generic function satisfies an `impl Fn` bound") {
    CHECK(helpers::compile_and_run(R"(
        const apply = fn(g: impl Fn(n: i32): i32, v: i32): i32 { return g(v); };
        const holds = fn(g: impl Fn(x: auto): bool, v: auto): bool { return g(v); };

        pub const main = fn(): i32 {
            let lim: i32 = 3;
            let add = fn(x: auto): i32 { return @as(i32, @intCast(x)) + lim; };
            let above = fn(x: auto): bool { return x > lim; };
            let ok = holds(above, @as(i32, 5)) and !holds(above, @as(i32, 1));
            return apply(add, 4) + if (ok) 100 else 0;
        };
    )") == 7 + 100);
}

TEST_CASE("A generic function becomes the erased `fn` its signature asks for") {
    CHECK(helpers::compile_and_run(R"(
        const Op = struct { f: fn(n: i32): i32 };
        const apply = fn(g: fn(n: i32): i32, v: i32): i32 { return g(v); };
        const apply_ptr = fn(g: ^fn(n: i32): i32, v: i32): i32 { return g(v); };
        const twice = fn(x: auto): @TypeOf(x) { return x * 2; };

        pub const main = fn(): i32 {
            let off: i32 = 2;
            let g = fn(x: auto): i32 { return @as(i32, @intCast(x)) + off; };
            let mut h: fn(n: i32): i32 = g;
            let first = h(1);
            h = twice;
            let p: ^fn(n: u8): i32 = g;
            let op: Op = .{ .f = g };
            let fs = [2]fn(n: i32): i32{ g, twice };
            return first + h(2) + p(@as(u8, 3)) + op.f(4) + fs[0](5) + fs[1](6) + apply(g, 7) +
                   apply_ptr(twice, 8);
        };
    )") == 3 + 4 + 5 + 6 + 7 + 12 + 9 + 16);
}

TEST_CASE("A generic function that doesn't fit the expected signature is rejected") {
    CHECK(helpers::raised(R"(
        const apply = fn(g: fn(n: i32): i32, v: i32): i32 { return g(v); };
        pub const main = fn(): i32 {
            let f = fn(x: auto): bool { return x > 0; };
            return apply(f, 4);
        };
    )",
                          sema::error::CLOSURE_SIGNATURE_MISMATCH));
}

TEST_CASE("A generic function is a `comptime` function argument when it captures nothing") {
    CHECK(helpers::compile_and_run(R"(
        const apply = fn(comptime g: fn(n: i32): i32, v: i32): i32 { return g(v); };
        const twice = fn(x: auto): @TypeOf(x) { return x * 2; };
        pub const main = fn(): i32 { return apply(twice, 21); };
    )") == 42);
    CHECK(helpers::raised(R"(
        const apply = fn(comptime g: fn(n: i32): i32, v: i32): i32 { return g(v); };
        pub const main = fn(): i32 {
            let off: i32 = 1;
            let f = fn(x: auto): i32 { return @as(i32, @intCast(x)) + off; };
            return apply(f, 4);
        };
    )",
                          sema::error::COMPTIME_EVALUATION_FAILED));
}

TEST_CASE("A generic function is returned by its own type") {
    CHECK(helpers::compile_and_run(R"(
        const make_plain = fn(): auto {
            return fn(x: auto): i32 { return @as(i32, @intCast(x)) * 2; };
        };
        const make_adder = fn(off: i32): auto {
            return fn(x: auto): i32 { return @as(i32, @intCast(x)) + off; };
        };
        const apply = fn(g: auto, v: i32): i32 { return g(v); };

        pub const main = fn(): i32 {
            let p = make_plain();
            let a = make_adder(3);
            return p(@as(u8, 4)) + a(@as(u8, 4)) + a(@as(i64, 5)) + apply(make_adder(10), 5);
        };
    )") == 8 + 7 + 8 + 15);
}

TEST_CASE("A returned generic `move fn` keeps its own state across calls") {
    CHECK(helpers::compile_and_run(R"(
        const counter = fn(): auto {
            let mut n: i32 = 0;
            return move fn(x: auto): i32 { n += @intCast(x); return n; };
        };

        pub const main = fn(): i32 {
            let f = counter();
            _ = f(@as(u8, 4));
            return f(@as(i64, 5));
        };
    )") == 9);
}

TEST_CASE("A generic function returned as an erased `fn` captures nothing") {
    CHECK(helpers::compile_and_run(R"(
        const IntFn = fn(n: i32): i32;
        const make = fn(): IntFn {
            return fn(x: auto): i32 { return @as(i32, @intCast(x)) * 3; };
        };
        pub const main = fn(): i32 { let f = make(); return f(4); };
    )") == 12);
}

TEST_CASE("A returned generic function can't keep a reference into the returning frame") {
    CHECK(helpers::raised(R"(
        const make = fn(): auto {
            let mut n: i32 = 0;
            return fn(x: auto): i32 { n += @intCast(x); return n; };
        };
        pub const main = fn(): i32 { let f = make(); return f(@as(u8, 4)); };
    )",
                          sema::error::ILLEGAL_CLOSURE_ESCAPE));
    CHECK(helpers::raised(R"(
        const IntFn = fn(n: i32): i32;
        const make = fn(off: i32): IntFn {
            return move fn(x: auto): i32 { return @as(i32, @intCast(x)) + off; };
        };
        pub const main = fn(): i32 { let f = make(3); return f(4); };
    )",
                          sema::error::ILLEGAL_CLOSURE_ESCAPE));
}

TEST_CASE("`@fnCtx()` in a generic body names the generic itself") {
    CHECK(helpers::compile_and_run(R"(
        const sum_to = fn(x: auto): @TypeOf(x) {
            if (x == 0) { return 0; }
            return x + @fnCtx()(x - 1);
        };
        pub const main = fn(): i32 { return sum_to(@as(i32, 4)); };
    )") == 10);
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut total: i32 = 0;
            let f = fn(x: auto): i32 {
                total += 1;
                if (x == 0) { return 0; }
                return @intCast(x + @fnCtx()(x - 1));
            };
            return f(@as(i32, 4)) + total;
        };
    )") == 10 + 5);
}

} // namespace ghoti::tests
