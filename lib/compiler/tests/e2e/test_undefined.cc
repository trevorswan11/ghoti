#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`= undefined` leaves a scalar local uninitialized but usable once written") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut x: i32 = undefined;
            x = 42;
            return x;
        };
    )") == 42);
}

TEST_CASE("`= undefined` allocates an aggregate local without a store") {
    CHECK(helpers::compile_and_run(R"(
        const Point = struct { x: i32, y: i32 };
        pub const main = fn(): i32 {
            let mut p: Point = undefined;
            p.x = 40;
            p.y = 2;
            return p.x + p.y;
        };
    )") == 42);
}

TEST_CASE("`= undefined` works for an array local written through mutable elements") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut buf: [3uz]mut i32 = undefined;
            buf[0] = 10;
            buf[1] = 20;
            buf[2] = 12;
            return buf[0] + buf[1] + buf[2];
        };
    )") == 42);
}

TEST_CASE("a `const` alias of `undefined` initializes like the literal") {
    CHECK(helpers::compile_and_run(R"(
        const U = undefined;
        const S = struct { a: i32, b: i32 };
        pub const main = fn(): i32 {
            const L = undefined;
            let mut x: i32 = U;
            x = 3;
            let mut s: S = L;
            s.a = 5;
            let mut arr: [3]mut i32 = .{ 1, U, 3 };
            arr[1] = 2;
            x = U;
            x = 3;
            return x + s.a + arr[0] + arr[1] + arr[2];
        };
    )") == 14);
}

TEST_CASE("`@TypeOf(undefined)` names the type of `undefined` and its aliases") {
    CHECK(helpers::compile_and_run(R"(
        const U: @TypeOf(undefined) = undefined;
        pub const main = fn(): i32 {
            if comptime (@TypeOf(U) == @TypeOf(undefined)) { return 1; }
            return 2;
        };
    )") == 1);
}

TEST_CASE("`undefined` has no runtime representation outside a `const` binding") {
    CHECK(helpers::raised("pub const main = fn(): i32 { let mut u = undefined; return 0; };",
                          sema::error::COMPILE_TIME_ONLY_VALUE));
    CHECK(helpers::raised("let mut g: @TypeOf(undefined) = undefined;",
                          sema::error::COMPILE_TIME_ONLY_VALUE));
    CHECK(helpers::raised("const f = fn(x: @TypeOf(undefined)): void {};",
                          sema::error::COMPILE_TIME_ONLY_VALUE));
    CHECK(helpers::raised("const g = fn(): @TypeOf(undefined) { return undefined; };",
                          sema::error::COMPILE_TIME_ONLY_VALUE));
    CHECK(helpers::raised("const S = struct { u: @TypeOf(undefined) };",
                          sema::error::COMPILE_TIME_ONLY_VALUE));
    CHECK(helpers::raised(R"(
        const A = [2]@TypeOf(undefined);
        pub const main = fn(): i32 { let mut a: A = undefined; return 0; };
    )",
                          sema::error::COMPILE_TIME_ONLY_VALUE));
}

} // namespace ghoti::tests
