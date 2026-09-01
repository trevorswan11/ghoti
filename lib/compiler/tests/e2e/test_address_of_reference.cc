#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("E2E: `^r` on a `&i32` binding is a `^i32` aliasing the referent") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 40;
            const r: &i32 = &x;
            const p: ^i32 = ^r;
            return *p + 2;
        };
    )") == 42);
}

TEST_CASE("E2E: writing through `^mut r` mutates the reference's original target") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 0;
            var r: &mut i32 = &mut x;
            const p: ^mut i32 = ^mut r;
            p[0] = 42;
            return x;
        };
    )") == 42);
}

TEST_CASE("E2E: `^mut c` on a `&mut i32` parameter aliases the caller's variable") {
    CHECK(helpers::compile_and_run(R"(
        const bump := fn(c: &mut i32): void {
            const p: ^mut i32 = ^mut c;
            p[0] = p[0] + 1;
        };
        pub const main := fn(): i32 {
            var x: i32 = 41;
            bump(&mut x);
            return x;
        };
    )") == 42);
}

TEST_CASE("E2E: `^` of a reference-typed struct field points at the field's referent") {
    CHECK(helpers::compile_and_run(R"(
        const Holder := struct { r: &i32 };
        pub const main := fn(): i32 {
            const v: i32 = 40;
            const h := Holder{ .r = &v };
            const p: ^i32 = ^(h.r);
            return *p + 2;
        };
    )") == 42);
}

TEST_CASE("E2E: `^r` decays through the reference -- pointer round-trips as `^T`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 40;
            const r: &i32 = &x;
            const p := ^r;
            return *p + 2;
        };
    )") == 42);
}

} // namespace ghoti::tests
