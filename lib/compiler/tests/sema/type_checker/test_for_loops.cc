#include <catch2/catch_test_macros.hpp>

#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Illegal usage of const capture") {
    SECTION("By const reference") {
        helpers::expect_compile_error(R"(
            const map := fn(T: type, arr: []mut T, Ctx: type, func: fn(x: T, c: Ctx): T, ctx: Ctx): void {
                for (arr) |&v| {
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
        )");
    }

    SECTION("By const pointer") {
        helpers::expect_compile_error(R"(
        const map := fn(T: type, arr: []mut T, Ctx: type, func: fn(x: T, c: Ctx): T, ctx: Ctx): void {
            for (arr) |^v| {
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
        };)");
    }
}

TEST_CASE("A plain (no-modifier) capture is always read-only") {
    SECTION("Array element, mutable array") {
        helpers::expect_compile_error(R"(
    pub const main := fn(): void {
        var arr := [_]mut i32{1, 2, 3, 4};
        for (arr) |v| { v = 0; }
    };)");
    }

    SECTION("Range counter") {
        helpers::expect_compile_error(R"(
    pub const main := fn(): void {
        for (0..4) |i| { i = 0; }
    };)");
    }
}

TEST_CASE("Attempted mutable capture of const value in capture clause") {
    SECTION("By const reference") {
        helpers::expect_compile_error(R"(
    pub const main := fn(): void {
        const arr := [_]i32{1, 2, 3, 4};
        for (arr) |&mut a| {}
    };)");
    }

    SECTION("By const pointer") {
        helpers::expect_compile_error(R"(
    pub const main := fn(): void {
        const arr := [_]i32{1, 2, 3, 4};
        for (arr) |^mut a| {}
    };)");
    }
}

} // namespace ghoti::tests
