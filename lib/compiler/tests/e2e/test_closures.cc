#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("A value-capturing closure is called directly and observes the captured value") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var offset: i32 = 10;
            const add := fn(x: i32): i32 {
                return x + offset;
            };
            return add(5);
        };
    )") == 15);
}

TEST_CASE("A mutable-reference-capturing closure mutates the enclosing local") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var counter: i32 = 0;
            const bump := fn(): void {
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
        const apply := fn(T: type, val: T, func: fn(T): T): T {
            return func(val);
        };

        pub const main := fn(): i32 {
            var offset: i32 = 7;
            const add_offset := fn(x: i32): i32 {
                return x + offset;
            };
            return apply(i32, 5, add_offset);
        };
    )") == 12);
}

TEST_CASE("A plain function still works when passed to a fn(T): U generic parameter") {
    CHECK(helpers::compile_and_run(R"(
        const apply := fn(T: type, val: T, func: fn(T): T): T {
            return func(val);
        };

        const double_it := fn(x: i32): i32 {
            return x * 2;
        };

        pub const main := fn(): i32 {
            return apply(i32, 21, double_it);
        };
    )") == 42);
}

TEST_CASE("map over a slice with Ctx parameter") {
    SECTION("By mutable reference") {
        CHECK(helpers::compile_and_run(R"(
        const map := fn(T: type, arr: []mut T, Ctx: type, func: fn(T, Ctx): T, ctx: Ctx): void {
            for (arr) |&mut v| {
                v = func(v, ctx);
            }
        };

        const MyCtx := struct { offset: i32 };

        pub const main := fn(): i32 {
            const arr := [_]mut i32{1, 2, 3, 4};
            map(i32, arr, MyCtx, fn(x: i32, ctx: MyCtx): i32 {
                return x + ctx.offset;
            }, MyCtx{ .offset = 10 });
            return arr[0] + arr[1] + arr[2] + arr[3];
        };
    )") == 50);
    }

    SECTION("By mutable pointer") {
        CHECK(helpers::compile_and_run(R"(
        const map := fn(T: type, arr: []mut T, Ctx: type, func: fn(T, Ctx): T, ctx: Ctx): void {
            for (arr) |^mut v| {
                *v = func(*v, ctx);
            }
        };

        const MyCtx := struct { offset: i32 };

        pub const main := fn(): i32 {
            const arr := [_]mut i32{1, 2, 3, 4};
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
        const map := fn(T: type, arr: []mut T, func: fn(T): T): void {
            var i: usize = 0;
            while (i < arr.len) : (i += 1uz) {
                arr[i] = func(arr[i]);
            };
        };

        pub const main := fn(): i32 {
            var arr := [4]mut i32{1, 2, 3, 4};
            const offset: i32 = 10;
            map(i32, arr, fn(x: i32): i32 {
                return x + offset;
            });
            return arr[0] + arr[1] + arr[2] + arr[3];
        };
    )") == 50);
}

TEST_CASE("A move fn is returned and called after its definition frame has exited") {
    CHECK(helpers::compile_and_run(R"(
        const make_adder := fn(): auto {
            var n: i32 = 10;
            return move fn(): i32 {
                n = n + 5;
                return n;
            };
        };

        pub const main := fn(): i32 {
            const f := make_adder();
            return f();
        };
    )") == 15);
}

TEST_CASE("A non-move closure that mutates a captured variable cannot be returned (e2e)") {
    helpers::expect_compile_error(R"(
        const make_adder := fn(): auto {
            var n: i32 = 10;
            return fn(): i32 {
                n = n + 5;
                return n;
            };
        };

        pub const main := fn(): i32 {
            const f := make_adder();
            return f();
        };
    )");
}

TEST_CASE("A captured value forwarded through an intermediate function is read correctly") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var offset: i32 = 7;
            const middle := fn(): i32 {
                const inner := fn(x: i32): i32 {
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
        pub const main := fn(): i32 {
            var n: i32 = 0;
            const middle := fn(): void {
                const inner := fn(): void {
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
        pub const main := fn(): i32 {
            var n: i32 = 100;
            const level1 := fn(): i32 {
                const level2 := fn(): i32 {
                    const level3 := fn(): i32 {
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
        const double_it := fn(x: i32): i32 {
            return x * 2;
        };
        const square_it := fn(x: i32): i32 {
            return x * x;
        };
        const negate_it := fn(x: i32): i32 {
            return -x;
        };

        pub const main := fn(): i32 {
            const fns := [3]fn(i32): i32{double_it, square_it, negate_it};
            return fns[0](3) + fns[1](3) + fns[2](3);
        };
    )") == 6 + 9 + -3);
}

TEST_CASE("A closure's .thunk can be called directly with its own environment") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var offset: i32 = 7;
            const add := fn(x: i32): i32 {
                return x + offset;
            };
            const thunk := add.thunk;
            return thunk(&mut add, 5);
        };
    )") == 12);
}

TEST_CASE("A closure's own address survives a round trip through an opaque pointer") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var offset: i32 = 7;
            const add := fn(x: i32): i32 {
                return x + offset;
            };
            const direct: usize = @intFromPtr(^mut add);
            const erased: ^mut opaque = @ptrCast(^mut opaque, ^mut add);
            const round_tripped: usize = @intFromPtr(erased);
            if (direct != round_tripped) {
                return -1;
            }
            return add(5);
        };
    )") == 12);
}

TEST_CASE("A top-level function recurses by calling its own name") {
    CHECK(helpers::compile_and_run(R"(
        const fact := fn(n: i32): i32 {
            if (n <= 1) {
                return 1;
            }
            return n * fact(n - 1);
        };

        pub const main := fn(): i32 {
            return fact(5);
        };
    )") == 120);
}

TEST_CASE("A local function recurses by calling its own name") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const fact := fn(n: i32): i32 {
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
        const fib := fn(n: i32): i32 {
            if (n <= 1) {
                return n;
            }
            return @fnCtx()(n - 1) + @fnCtx()(n - 2);
        };

        pub const main := fn(): i32 {
            return fib(10);
        };
    )") == 55);
}

TEST_CASE("A capturing closure recurses through @fnCtx()") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var calls: i32 = 0;
            const fact := fn(n: i32): i32 {
                calls = calls + 1;
                if (n <= 1) {
                    return 1;
                }
                return n * @fnCtx()(n - 1);
            };
            const result := fact(5);
            return result + calls;
        };
    )") == 125);
}

TEST_CASE("A closure cannot recurse by calling its own name") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            var offset: i32 = 1;
            const fact := fn(n: i32): i32 {
                if (n <= 1) {
                    return offset;
                }
                return n * fact(n - 1);
            };
            return fact(5);
        };
    )");
}

} // namespace ghoti::tests
