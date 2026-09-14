#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`^.{...}` decays a const-evaluatable literal into a slice returned from a function") {
    CHECK(helpers::compile_and_run(R"(
        const get_slice := fn(): []i32 {
            return ^.{1, 2, 3};
        };
        pub const main := fn(): i32 {
            const s := get_slice();
            if (s.len != 3uz) { return -1; }
            return s[0] + s[1] + s[2];
        };
    )") == 6);
}

TEST_CASE("`^.{...}` slice survives being returned past its originating call frame") {
    CHECK(helpers::compile_and_run(R"(
        const get_slice := fn(): []i32 {
            return ^.{7, 8, 9};
        };
        const first := fn(s: []i32): i32 {
            return s[0];
        };
        pub const main := fn(): i32 {
            return first(get_slice());
        };
    )") == 7);
}

TEST_CASE("`^.{...}` works with runtime (non-constant) element values") {
    CHECK(helpers::compile_and_run(R"(
        const make := fn(x: i32, y: i32): []i32 {
            return ^.{x, y, x + y};
        };
        pub const main := fn(): i32 {
            const s := make(10, 20);
            return s[0] + s[1] + s[2];
        };
    )") == 60);
}

TEST_CASE("`^.{...}` works as a direct call argument") {
    CHECK(helpers::compile_and_run(R"(
        const sum := fn(s: []i32): i32 {
            return s[0] + s[1] + s[2];
        };
        pub const main := fn(): i32 {
            return sum(^.{1, 2, 3});
        };
    )") == 6);
}

TEST_CASE("`^.{...}` works for a local slice-typed decl") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const s: []i32 = ^.{4, 5, 6};
            return s[0] + s[1] + s[2];
        };
    )") == 15);
}

TEST_CASE("`^mut .{...}` is rejected; the sugar only applies to a `const`-element slice") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            const s: []mut i32 = ^mut .{1, 2, 3};
            return s[0];
        };
    )");
}

TEST_CASE("Ordinary `^expr` on a real local variable is unaffected by the literal-decay sugar") {
    CHECK(helpers::compile_and_run(R"(
        const deref := fn(p: ^i32): i32 {
            return *p;
        };
        pub const main := fn(): i32 {
            const x: i32 = 42;
            return deref(^x);
        };
    )") == 42);
}

} // namespace ghoti::tests
