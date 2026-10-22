#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("A value-capturing closure is called directly and observes the captured value") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut offset: i32 = 10;
            let add = fn(x: i32): i32 {
                return x + offset;
            };
            return add(5);
        };
    )") == 15);
}

TEST_CASE("A mutable-reference-capturing closure mutates the enclosing local") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut counter: i32 = 0;
            let bump = fn(): void {
                counter = counter + 1;
            };
            bump();
            bump();
            bump();
            return counter;
        };
    )") == 3);
}

TEST_CASE("A capturing closure is passed to a fn(T): U generic parameter and called through it") {
    CHECK(helpers::compile_and_run(R"(
        const apply = fn(comptime T: type, val: T, func: fn(x: T): T): T {
            return func(val);
        };

        pub const main = fn(): i32 {
            let mut offset: i32 = 7;
            let add_offset = fn(x: i32): i32 {
                return x + offset;
            };
            return apply(i32, 5, add_offset);
        };
    )") == 12);
}

TEST_CASE("A plain function still works when passed to a fn(T): U generic parameter") {
    CHECK(helpers::compile_and_run(R"(
        const apply = fn(comptime T: type, val: T, func: fn(x: T): T): T {
            return func(val);
        };

        const double_it = fn(x: i32): i32 {
            return x * 2;
        };

        pub const main = fn(): i32 {
            return apply(i32, 21, double_it);
        };
    )") == 42);
}

TEST_CASE("map over a slice with Ctx parameter") {
    SECTION("By mutable reference") {
        CHECK(helpers::compile_and_run(R"(
        const map = fn(comptime T: type, arr: []mut T, comptime Ctx: type, func: fn(x: T, c: Ctx): T, ctx: Ctx): void {
            for (arr) |&mut v| {
                v = func(v, ctx);
            }
        };

        const MyCtx = struct { offset: i32 };

        pub const main = fn(): i32 {
            let arr = [_]mut i32{1, 2, 3, 4};
            map(i32, arr, MyCtx, fn(x: i32, ctx: MyCtx): i32 {
                return x + ctx.offset;
            }, MyCtx{ .offset = 10 });
            return arr[0] + arr[1] + arr[2] + arr[3];
        };
    )") == 50);
    }

    SECTION("By mutable pointer") {
        CHECK(helpers::compile_and_run(R"(
        const map = fn(comptime T: type, arr: []mut T, comptime Ctx: type, func: fn(x: T, c: Ctx): T, ctx: Ctx): void {
            for (arr) |^mut v| {
                *v = func(*v, ctx);
            }
        };

        const MyCtx = struct { offset: i32 };

        pub const main = fn(): i32 {
            let arr = [_]mut i32{1, 2, 3, 4};
            map(i32, arr, MyCtx, fn(x: i32, ctx: MyCtx): i32 {
                return x + ctx.offset;
            }, MyCtx{ .offset = 10 });
            return arr[0] + arr[1] + arr[2] + arr[3];
        };
    )") == 50);
    }
}

TEST_CASE("map over a slice with a capturing closure, no explicit Ctx parameter needed") {
    CHECK(helpers::compile_and_run(R"(
        const map = fn(comptime T: type, arr: []mut T, func: fn(x: T): T): void {
            let mut i: usize = 0;
            while (i < arr.len) : (i += 1uz) {
                arr[i] = func(arr[i]);
            };
        };

        pub const main = fn(): i32 {
            let mut arr = [4]mut i32{1, 2, 3, 4};
            let offset: i32 = 10;
            map(i32, arr, fn(x: i32): i32 {
                return x + offset;
            });
            return arr[0] + arr[1] + arr[2] + arr[3];
        };
    )") == 50);
}

TEST_CASE("A move fn is returned and called after its definition frame has exited") {
    CHECK(helpers::compile_and_run(R"(
        const make_adder = fn(): auto {
            let mut n: i32 = 10;
            return move fn(): i32 {
                n = n + 5;
                return n;
            };
        };

        pub const main = fn(): i32 {
            let f = make_adder();
            return f();
        };
    )") == 15);
}

TEST_CASE("A non-move closure that mutates a captured variable cannot be returned (e2e)") {
    helpers::expect_compile_error(R"(
        const make_adder = fn(): auto {
            let mut n: i32 = 10;
            return fn(): i32 {
                n = n + 5;
                return n;
            };
        };

        pub const main = fn(): i32 {
            let f = make_adder();
            return f();
        };
    )");
}

TEST_CASE("A non-move closure bound to a local before being returned still cannot escape (e2e)") {
    helpers::expect_compile_error(R"(
        const make_adder = fn(): auto {
            let mut n: i32 = 10;
            const g = fn(): i32 {
                n = n + 5;
                return n;
            };
            return g;
        };

        pub const main = fn(): i32 {
            let f = make_adder();
            return f();
        };
    )");
}

TEST_CASE("A captured value forwarded through an intermediate function is read correctly") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut offset: i32 = 7;
            let middle = fn(): i32 {
                let inner = fn(x: i32): i32 {
                    return x + offset;
                };
                return inner(5);
            };
            return middle();
        };
    )") == 12);
}

TEST_CASE("A mutation forwarded through an intermediate function reaches the true owner") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut n: i32 = 0;
            let middle = fn(): void {
                let inner = fn(): void {
                    n = n + 1;
                };
                inner();
                inner();
            };
            middle();
            return n;
        };
    )") == 2);
}

TEST_CASE("A capture is forwarded correctly through three levels of nesting") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut n: i32 = 100;
            let level1 = fn(): i32 {
                let level2 = fn(): i32 {
                    let level3 = fn(): i32 {
                        return n;
                    };
                    return level3();
                };
                return level2();
            };
            return level1();
        };
    )") == 100);
}

TEST_CASE("An array can hold distinct non-capturing functions sharing one signature") {
    CHECK(helpers::compile_and_run(R"(
        const double_it = fn(x: i32): i32 {
            return x * 2;
        };
        const square_it = fn(x: i32): i32 {
            return x * x;
        };
        const negate_it = fn(x: i32): i32 {
            return -x;
        };

        pub const main = fn(): i32 {
            let fns = [3]fn(n: i32): i32{double_it, square_it, negate_it};
            return fns[0](3) + fns[1](3) + fns[2](3);
        };
    )") == 6 + 9 + -3);
}

TEST_CASE("A closure's .thunk can be called directly with its own environment") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut offset: i32 = 7;
            let add = fn(x: i32): i32 {
                return x + offset;
            };
            let thunk = add.thunk;
            return thunk(&mut add, 5);
        };
    )") == 12);
}

TEST_CASE("A closure's own address survives a round trip through an opaque pointer") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut offset: i32 = 7;
            let add = fn(x: i32): i32 {
                return x + offset;
            };
            let direct: usize = @intFromPtr(^mut add);
            let erased: ^mut opaque = @ptrCast(^mut opaque, ^mut add);
            let round_tripped: usize = @intFromPtr(erased);
            if (direct != round_tripped) {
                return -1;
            }
            return add(5);
        };
    )") == 12);
}

TEST_CASE("A top-level function recurses by calling its own name") {
    CHECK(helpers::compile_and_run(R"(
        const fact = fn(n: i32): i32 {
            if (n <= 1) {
                return 1;
            }
            return n * fact(n - 1);
        };

        pub const main = fn(): i32 {
            return fact(5);
        };
    )") == 120);
}

TEST_CASE("A local function recurses by calling its own name") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            const fact = fn(n: i32): i32 {
                if (n <= 1) {
                    return 1;
                }
                return n * fact(n - 1);
            };
            return fact(5);
        };
    )") == 120);
}

TEST_CASE("A non-capturing function recurses through @fnCtx()") {
    CHECK(helpers::compile_and_run(R"(
        const fib = fn(n: i32): i32 {
            if (n <= 1) {
                return n;
            }
            return @fnCtx()(n - 1) + @fnCtx()(n - 2);
        };

        pub const main = fn(): i32 {
            return fib(10);
        };
    )") == 55);
}

TEST_CASE("A capturing closure recurses through @fnCtx()") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut calls: i32 = 0;
            let fact = fn(n: i32): i32 {
                calls = calls + 1;
                if (n <= 1) {
                    return 1;
                }
                return n * @fnCtx()(n - 1);
            };
            let result = fact(5);
            return result + calls;
        };
    )") == 125);
}

TEST_CASE("A closure cannot recurse by calling its own name") {
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            let mut offset: i32 = 1;
            const fact = fn(n: i32): i32 {
                if (n <= 1) {
                    return offset;
                }
                return n * fact(n - 1);
            };
            return fact(5);
        };
    )");
}

TEST_CASE("A function taking a function pointer executes passed functions directly") {
    CHECK(helpers::compile_and_run(R"(
        const apply = fn(func: ^fn(x: i32): i32, val: i32): i32 {
            return func(val);
        };

        const double_it = fn(x: i32): i32 {
            return x * 2;
        };

        const square_it = fn(x: i32): i32 {
            return x * x;
        };

        pub const main = fn(): i32 {
            return apply(double_it, 5) + apply(square_it, 5);
        };
    )") == 10 + 25);
}

TEST_CASE("a capturing function literal can be called where it is written") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let offset: i32 = 10;
            let mut total: i32 = 1;
            let read = fn(x: i32): i32 { return x + offset; }(5);
            let bumped = fn(x: i32): i32 { total += x; return total; }(4);
            return read + bumped + total;
        };
    )") == 15 + 5 + 5);
}

TEST_CASE("a closure in a test block captures the test's locals") {
    CHECK(helpers::compile_and_run_tests(R"(
        test "reads" {
            let a: i32 = 2;
            let c = fn(): i32 { return a; };
            @expect(c() == 2);
        }
        test "writes" {
            let mut a: i32 = 2;
            let c = fn(): i32 { a += 1; return a; };
            @expect(c() == 3);
            @expect(a == 3);
        }
    )") == 0);
}

TEST_CASE("a type first used inside a function doesn't make its methods closures") {
    CHECK(helpers::compile_and_run(R"(
        const S = struct { v: i32 };
        const run = fn(): i32 {
            let local: i32 = 40;
            let mut s: S = .{ .v = local };
            X.bump(&mut s);
            return s.v;
        };
        const X = struct {
            const two = fn(): i32 { return 2; };
            const bump = fn(s: &mut S): void {
                let by = two();
                s.v += by;
            };
        };
        pub const main = fn(): i32 { return run(); };
    )") == 42);
}

TEST_CASE("A closure reads and writes several captures at their own environment slots") {
    CHECK(helpers::compile_and_run(R"(
        const count_above = fn(xs: []i32, lim: i32): i32 {
            let mut n: i32 = 0;
            let check = fn(v: i32): void { if (v > lim) { n += 1; } };
            for (xs) |v| { check(v); }
            return n;
        };

        pub const main = fn(): i32 {
            let a = [_]i32{1, 5, 9, 12};
            return count_above(a[0..], 4);
        };
    )") == 3);

    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let a: i32 = 1;
            let b: i32 = 20;
            let mut hits: i32 = 0;
            let f = fn(n: i32): i32 { hits += 1; return n * b + a; };
            return f(2) + f(3) + hits;
        };
    )") == 41 + 61 + 2);
}

TEST_CASE("A closure reads its enclosing function's type aliases and constants uncaptured") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            const T = i32;
            const k = 4;
            let base: T = 3;
            let f = fn(y: T): T { return y * k + base + @as(T, @sizeOf(T)); };
            return f(2);
        };
    )") == 15);
}

TEST_CASE("A closure captures parameters it never mutates, including a slice") {
    CHECK(helpers::compile_and_run(R"(
        const sum = fn(xs: []i32, bias: i32): i32 {
            let mut acc: i32 = 0;
            let add = fn(i: usize): void { acc += xs[i] + bias; };
            for (0..xs.len) |i| { add(i); }
            return acc;
        };

        pub const main = fn(): i32 {
            let a = [_]i32{1, 2, 3};
            return sum(a[0..], 1);
        };
    )") == 9);
}

TEST_CASE("A closure inside a method captures `self` rather than its own environment") {
    CHECK(helpers::compile_and_run(R"(
        const Acc = struct {
            v: i32,
            const add_all = fn(&self, xs: []i32): i32 {
                let mut acc: i32 = 0;
                let add = fn(x: i32): void { acc += x + self.v; };
                for (xs) |x| { add(x); }
                return acc;
            };
        };

        pub const main = fn(): i32 {
            let a: Acc = .{ .v = 10 };
            let xs = [_]i32{1, 2, 3};
            return a.add_all(xs[0..]);
        };
    )") == 36);
}

TEST_CASE("A move fn keeps writes to its own copies across calls") {
    CHECK(helpers::compile_and_run(R"(
        const counter = fn(start: i32, step: i32): auto {
            let mut cur: i32 = start;
            return move fn(): i32 { cur += step; return cur; };
        };

        pub const main = fn(): i32 {
            let c = counter(0, 2);
            _ = c();
            _ = c();
            return c();
        };
    )") == 6);
}

TEST_CASE("Distinct closures sharing a signature each get their own generic instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const call = fn(g: impl Fn(n: i32): i32, v: i32): i32 { return g(v); };
        const call_auto = fn(g: auto, v: i32): i32 { return g(v); };

        pub const main = fn(): i32 {
            let a: i32 = 10;
            let b: i32 = 20;
            let x = call(fn(n: i32): i32 { return n + a; }, 1);
            let y = call(fn(n: i32): i32 { return n * b; }, 2);
            let z = call_auto(fn(n: i32): i32 { return n - a; }, 15);
            let w = call_auto(fn(n: i32): i32 { return n - b; }, 25);
            return x + y + z + w;
        };
    )") == 11 + 40 + 5 + 5);
}

} // namespace ghoti::tests
