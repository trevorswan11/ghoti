#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/sema/error.hh"

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("A closure in a generic body captures a parameter of the instantiated type") {
    CHECK(helpers::compile_and_run(R"(
        const add_to = fn(comptime T: type, base: T, x: T): T {
            let f = fn(y: T): T { return y + base; };
            return f(x);
        };

        pub const main = fn(): i32 {
            return @intCast(add_to(i64, 20, 7));
        };
    )") == 27);
}

TEST_CASE("Each instantiation of a generic types its closures on their own") {
    CHECK(helpers::compile_and_run(R"(
        const add_to = fn(comptime T: type, base: T, x: T): T {
            let f = fn(y: T): T { return y + base; };
            return f(x);
        };

        pub const main = fn(): i32 {
            let a = add_to(i32, 10, 5);
            let b = add_to(i64, 20, 7);
            return a + @as(i32, @intCast(b));
        };
    )") == 15 + 27);
}

TEST_CASE("Each instantiation of a generic types its non-capturing local functions on their own") {
    CHECK(helpers::compile_and_run(R"(
        const twice = fn(comptime T: type, x: T): T {
            let f = fn(y: T): T { return y + y; };
            return f(x);
        };

        pub const main = fn(): i32 {
            let a = twice(i32, 5);
            let b = twice(i64, 7);
            return a + @as(i32, @intCast(b));
        };
    )") == 10 + 14);
}

TEST_CASE("A closure in a generic body mutates a local of each instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const count_above = fn(comptime T: type, xs: []T, lim: T): i32 {
            let mut n: i32 = 0;
            let check = fn(v: T): void { if (v > lim) { n += 1; } };
            for (xs) |v| { check(v); }
            return n;
        };

        pub const main = fn(): i32 {
            let a = [_]i32{1, 5, 9, 12};
            let b = [_]u16{100, 200, 300};
            return count_above(i32, a[0..], 4) * 10 + count_above(u16, b[0..], 150);
        };
    )") == 32);
}

TEST_CASE("A closure in a generic body reads the instantiation's compile-time parameters") {
    CHECK(helpers::compile_and_run(R"(
        const scale_by = fn(comptime T: type, comptime k: T, x: T): T {
            let f = fn(y: T): T { return y * k + @as(T, @sizeOf(T)); };
            return f(x);
        };

        pub const main = fn(): i32 {
            return scale_by(i32, 3, 2) + @intCast(scale_by(u8, 2, 5));
        };
    )") == 10 + 11);
}

TEST_CASE("Nested closures in a generic body forward captures per instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const outer = fn(comptime T: type, a: T): T {
            let mut total: T = 0;
            let lvl1 = fn(b: T): void {
                let lvl2 = fn(): void { total += a + b; };
                lvl2();
                lvl2();
            };
            lvl1(1);
            return total;
        };

        pub const main = fn(): i32 {
            return outer(i32, 4) + @intCast(outer(i16, 2));
        };
    )") == 10 + 6);
}

TEST_CASE("A closure in a generic body is passed on to another generic") {
    CHECK(helpers::compile_and_run(R"(
        const apply = fn(comptime T: type, v: T, func: fn(x: T): T): T { return func(v); };
        const scaled = fn(comptime T: type, k: T, v: T): T {
            return apply(T, v, fn(x: T): T { return x * k; });
        };

        pub const main = fn(): i32 {
            return scaled(i32, 3, 4) + @intCast(scaled(u64, 2, 5));
        };
    )") == 12 + 10);
}

TEST_CASE("A move fn returned from each instantiation of a generic keeps its own state") {
    CHECK(helpers::compile_and_run(R"(
        const make_adder = fn(comptime T: type, n: T): auto {
            return move fn(x: T): T { return x + n; };
        };
        const counter = fn(comptime T: type, start: T, step: T): auto {
            let mut cur: T = start;
            return move fn(): T { cur += step; return cur; };
        };

        pub const main = fn(): i32 {
            let f = make_adder(i32, 10);
            let g = make_adder(u8, 3);
            let c = counter(i32, 0, 2);
            let d = counter(u32, 100, 1);
            _ = c();
            _ = d();
            return f(5) + @as(i32, g(4)) + c() + @as(i32, @intCast(d() - 100));
        };
    )") == 15 + 7 + 4 + 2);
}

TEST_CASE("A closure in a type constructor's method captures `self` and the element type") {
    CHECK(helpers::compile_and_run(R"(
        const Box = fn(comptime T: type): type {
            return struct {
                v: T,
                const add_all = fn(&self, xs: []T): T {
                    let mut acc: T = 0;
                    let add = fn(x: T): void { acc += x + self.v; };
                    for (xs) |x| { add(x); }
                    return acc;
                };
            };
        };

        pub const main = fn(): i32 {
            let b: Box(i32) = .{ .v = 1 };
            let c: Box(u8) = .{ .v = 2 };
            let xs = [_]i32{1, 2, 3};
            let ys = [_]u8{4, 5};
            return b.add_all(xs[0..]) + @intCast(c.add_all(ys[0..]));
        };
    )") == 9 + 13);
}

TEST_CASE("A generic local function reads the constants and types around it") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            const off: i32 = 10;
            const Out = i32;
            let f = fn(x: auto): Out { return @as(Out, @intCast(x)) + off; };
            let g = fn(comptime T: type, x: T): Out { return @as(Out, @intCast(x)) * off; };
            return f(@as(i64, 1)) + f(@as(u8, 2)) + g(u16, 3);
        };
    )") == 11 + 12 + 30);
}

TEST_CASE("A generic local function can't capture a runtime local") {
    struct shape {
        const char* params;
        const char* args;
    };
    for (const auto& [params, args] :
         {shape{"x: auto", "@as(i64, 1)"}, shape{"comptime T: type, x: T", "i64, 1"}}) {
        CAPTURE(params);
        CHECK(helpers::raised(fmt::format(R"(
            pub const main = fn(): i32 {{
                let off: i32 = 10;
                let f = fn({}): i32 {{ return @as(i32, @intCast(x)) + off; }};
                return f({});
            }};
        )",
                                          params,
                                          args),
                              sema::error::GENERIC_CLOSURE_CAPTURE));
    }
}

} // namespace ghoti::tests
