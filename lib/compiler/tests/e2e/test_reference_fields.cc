#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("A struct can store a &i32 field and read through it") {
    CHECK(helpers::compile_and_run(R"(
        const RefHolder := struct { r: &i32 };
        pub const main := fn(): i32 {
            let mut x: i32 = 41;
            let h := RefHolder{ .r = &x };
            return h.r + 1;
        };
    )") == 42);
}

TEST_CASE("A struct &mut i32 field writes through to the referent") {
    CHECK(helpers::compile_and_run(R"(
        const MutHolder := struct { r: &mut i32 };
        pub const main := fn(): i32 {
            let mut x: i32 = 5;
            let h := MutHolder{ .r = &mut x };
            *h.r = 99;
            return x;
        };
    )") == 99);
}

TEST_CASE("A reference struct field aliases the live value after it changes") {
    CHECK(helpers::compile_and_run(R"(
        const RefHolder := struct { r: &mut i32 };
        pub const main := fn(): i32 {
            let mut x: i32 = 1;
            let h := RefHolder{ .r = &mut x };
            x = 41;
            return h.r + 1;
        };
    )") == 42);
}

TEST_CASE("A nested struct reference field reads through two hops") {
    CHECK(helpers::compile_and_run(R"(
        const Inner := struct { r: &i32 };
        const Outer := struct { inner: Inner };
        pub const main := fn(): i32 {
            let mut x: i32 = 41;
            let o := Outer{ .inner = Inner{ .r = &x } };
            return o.inner.r + 1;
        };
    )") == 42);
}

TEST_CASE("A struct with a &mut field is passed by value and mutates its referent") {
    CHECK(helpers::compile_and_run(R"(
        const Sink := struct { out: &mut i32 };
        const bump := fn(s: Sink): void {
            *s.out = *s.out + 1;
        };
        pub const main := fn(): i32 {
            let mut x: i32 = 41;
            let s := Sink{ .out = &mut x };
            bump(s);
            return x;
        };
    )") == 42);
}

TEST_CASE("A function returns a struct carrying a reference into a caller-owned value") {
    CHECK(helpers::compile_and_run(R"(
        const Ref := struct { r: &i32 };
        const wrap := fn(v: &i32): Ref {
            return Ref{ .r = v };
        };
        pub const main := fn(): i32 {
            let mut x: i32 = 41;
            let w := wrap(&x);
            return w.r + 1;
        };
    )") == 42);
}

TEST_CASE("An extern struct or union may still hold a raw pointer field") {
    helpers::type_check_and_verify(R"(
        const CView := extern struct { data: ^u8, len: usize };
        const CPayload := extern union { as_ptr: ^u8, as_int: usize };
        const f := fn(v: CView, p: CPayload): usize {
            return v.len + p.as_int;
        };
    )");
}

} // namespace ghoti::tests
