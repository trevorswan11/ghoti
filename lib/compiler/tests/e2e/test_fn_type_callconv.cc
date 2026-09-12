#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("a `fn(...): T` type annotation accepts its own `callconv(.x)`") {
    CHECK(helpers::compile_and_run(R"(
        const add := fn(a: i32, b: i32) callconv(.sysv): i32 { return a + b; };
        pub const main := fn(): i32 {
            var fp: fn(a: i32, b: i32) callconv(.sysv): i32 = add;
            return 7;
        };
    )") == 7);
}

TEST_CASE("a `fn(...): T` type annotation with no `callconv` still defaults to `.c`") {
    CHECK(helpers::compile_and_run(R"(
        const add := fn(a: i32, b: i32): i32 { return a + b; };
        pub const main := fn(): i32 {
            var fp: fn(a: i32, b: i32): i32 = add;
            return fp(3, 4);
        };
    )") == 7);
}

TEST_CASE("known limitation: assigning a mismatched-callconv function is not yet rejected") {
    CHECK(helpers::compile_and_run(R"(
        const add := fn(a: i32, b: i32) callconv(.sysv): i32 { return a + b; };
        pub const main := fn(): i32 {
            var fp: fn(a: i32, b: i32) callconv(.c): i32 = add;
            return 0;
        };
    )") == 0);
}

} // namespace ghoti::tests
