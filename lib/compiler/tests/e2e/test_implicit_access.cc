#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("A no-self function member is called directly through implicit access") {
    CHECK(helpers::compile_and_run(R"(
        const T := struct {
            x: i32,

            const init := fn(): T {
                return T{ .x = 99 };
            };
        };

        pub const main := fn(): i32 {
            const a: T = .init();
            return a.x;
        };
    )") == 99);
}

TEST_CASE("A no-self function member is bound bare through implicit access and called later") {
    CHECK(helpers::compile_and_run(R"(
        const T := struct {
            x: i32,

            const init := fn(): T {
                return T{ .x = 42 };
            };
        };

        pub const main := fn(): i32 {
            const f: fn(): T = .init;
            const a := f();
            return a.x;
        };
    )") == 42);
}

TEST_CASE("A no-self function member taking arguments is called through implicit access") {
    CHECK(helpers::compile_and_run(R"(
        const T := struct {
            x: i32,

            const from := fn(v: i32): T {
                return T{ .x = v };
            };
        };

        pub const main := fn(): i32 {
            const a: T = .from(7);
            return a.x;
        };
    )") == 7);
}

TEST_CASE("Implicit access on the left side of a comparison is rejected") {
    helpers::expect_compile_error(R"(
        const U := union { a: i32, b: i32 };
        pub const main := fn(): i32 {
            var u := U{ .a = 1 };
            if (.a == u) { return 1; }
            return 0;
        };
    )");
}

} // namespace ghoti::tests
