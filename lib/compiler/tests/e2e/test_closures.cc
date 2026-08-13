#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

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
    SKIP("For loop mutability isnt working...");
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

} // namespace ghoti::tests
