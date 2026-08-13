#include <catch2/catch_test_macros.hpp>

#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Illegal usage of const capture") {
    SKIP("Bug with constant indirect iteration");
    SECTION("By const reference") {
        helpers::expect_compile_error(R"(
            const map := fn(T: type, arr: []mut T, Ctx: type, func: fn(T, Ctx): T, ctx: Ctx): void {
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
        const map := fn(T: type, arr: []mut T, Ctx: type, func: fn(T, Ctx): T, ctx: Ctx): void {
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
        };
    )");
    }
}

TEST_CASE("Attempted mutable capture of const value in capture clause") {
    SKIP("Bug? Or maybe just no error emitted");
    SECTION("By const reference") {
        helpers::expect_compile_error(R"(
    pub const main := fn(): void {
        const arr := [_]i32{1, 2, 3, 4};
        for (arr) |&mut a| {}
    };
)");
    }

    SECTION("By const pointer") {
        helpers::expect_compile_error(R"(
    pub const main := fn(): void {
        const arr := [_]i32{1, 2, 3, 4};
        for (arr) |^mut a| {}
    };
)");
    }
}

} // namespace ghoti::tests
