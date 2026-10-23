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

TEST_CASE("A generic local function captures runtime locals around it") {
    struct shape {
        const char* params;
        const char* first;
        const char* second;
    };
    for (const auto& [params, first, second] :
         {shape{"x: auto", "@as(i64, 1)", "@as(u8, 2)"},
          shape{"comptime T: type, x: T", "i64, 1", "u8, 2"}}) {
        CAPTURE(params);
        CHECK(helpers::compile_and_run(fmt::format(R"(
            pub const main = fn(): i32 {{
                let off: i32 = 10;
                let f = fn({}): i32 {{ return @as(i32, @intCast(x)) + off; }};
                return f({}) + f({});
            }};
        )",
                                                   params,
                                                   first,
                                                   second)) == 11 + 12);
    }
}

TEST_CASE("A generic local function mutates a captured `let mut` from every monomorph") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut hits: i32 = 0;
            let scale: i32 = 2;
            let f = fn(comptime T: type, x: T): T {
                hits += 1;
                return x * @as(T, @intCast(scale));
            };
            let a = f(i32, 5);
            let b = f(u8, 3);
            return a + @as(i32, b) + hits;
        };
    )") == 10 + 6 + 2);
}

TEST_CASE("A generic `move fn` keeps its own copies across calls and monomorphs") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut n: i32 = 0;
            let bump = move fn(x: auto): i32 { n += @intCast(x); return n; };
            _ = bump(@as(i32, 1));
            _ = bump(@as(u8, 2));
            return bump(@as(i32, 3)) + n;
        };
    )") == 6);
}

TEST_CASE("A capturing generic local function recurses by name") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let off: i32 = 1;
            let f = fn(x: auto): i32 {
                if (x == 0) { return off; }
                return f(x - 1) + off;
            };
            return f(@as(i32, 3));
        };
    )") == 4);
}

TEST_CASE("A capturing generic local function forwards captures to closures inside it") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let off: i32 = 5;
            let f = fn(x: auto): i32 {
                let g = fn(): i32 { return off + 1; };
                return g() + @as(i32, @intCast(x));
            };
            return f(@as(u8, 1));
        };
    )") == 7);
}

TEST_CASE("A closure or another generic calls a capturing generic local function") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let off: i32 = 5;
            let f = fn(x: auto): i32 { return off + @as(i32, @intCast(x)); };
            let g = fn(): i32 { return f(@as(u8, 2)) + f(@as(i64, 3)); };
            return g();
        };
    )") == 15);
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let a: i32 = 3;
            let f = fn(x: auto): i32 { return a * @as(i32, @intCast(x)); };
            let g = fn(y: auto): i32 { return f(y) + 1; };
            return g(@as(u8, 4)) + g(@as(i64, 1));
        };
    )") == 13 + 4);
}

TEST_CASE("A generic local function in a generic body captures that instantiation's locals") {
    CHECK(helpers::compile_and_run(R"(
        const outer = fn(comptime T: type, base: T): T {
            let f = fn(x: auto): T { return base + @as(T, @intCast(x)); };
            return f(@as(u8, 1)) + f(@as(i64, 2));
        };

        pub const main = fn(): i32 {
            return outer(i32, 10) + @as(i32, @intCast(outer(i64, 20)));
        };
    )") == 23 + 43);
}

TEST_CASE(
    "A generic local function captures parameters, nested-block locals, and `const` bindings") {
    CHECK(helpers::compile_and_run(R"(
        const S = struct { v: i32 };
        const run = fn(xs: []i32, s: S): i32 {
            let f = fn(i: auto): i32 { return xs[i] + s.v; };
            return f(@as(usize, 1)) + f(@as(usize, 2));
        };

        pub const main = fn(): i32 {
            let a = [_]i32{1, 2, 3};
            return run(a[0..], .{ .v = 10 });
        };
    )") == 25);
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let base: i32 = 2;
            let mut r: i32 = 0;
            if (base > 1) {
                let extra: i32 = 5;
                const f = fn(x: auto): i32 { return base + extra + @as(i32, @intCast(x)); };
                r = f(@as(u8, 1)) + f(@as(i64, 2));
            }
            return r;
        };
    )") == 17);
}

TEST_CASE("A generic local function in a test block captures the test's locals") {
    CHECK(helpers::compile_and_run_tests(R"(
        test "generic capture" {
            let off: i32 = 3;
            let f = fn(x: auto): i32 { return off + @as(i32, @intCast(x)); };
            @expect(f(@as(u8, 1)) == 4);
            @expect(f(@as(i64, 2)) == 5);
        }
    )") == 0);
}

TEST_CASE("A generic local function can't capture a binding declared after it") {
    CHECK(helpers::raised(R"(
        pub const main = fn(): i32 {
            let f = fn(x: auto): i32 { return later + @as(i32, @intCast(x)); };
            let later: i32 = 1;
            return f(@as(u8, 2));
        };
    )",
                          sema::error::GENERIC_CLOSURE_CAPTURE));
}

} // namespace ghoti::tests
