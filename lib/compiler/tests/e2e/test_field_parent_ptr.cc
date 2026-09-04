#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("`@fieldParentPtr` recovers a struct pointer from a first-member field pointer") {
    CHECK(helpers::compile_and_run(R"(
        const Inner := struct { a: i32, b: i32 };
        const Outer := struct { inner: Inner, tag: i32 };

        pub const main := fn(): i32 {
            var o: Outer = undefined;
            o.tag = 100;
            o.inner.a = 40;
            o.inner.b = 2;

            const ip: ^mut Inner = ^mut o.inner;
            const op: ^mut Outer = @fieldParentPtr(Outer, "inner", ip);
            return op.inner.a + op.inner.b;
        };
    )") == 42);
}

TEST_CASE("`@fieldParentPtr` recovers a struct pointer from a non-first-member field pointer") {
    CHECK(helpers::compile_and_run(R"(
        const Inner := struct { a: i32, b: i32 };
        const Outer := struct { lead: i64, pad: i32, inner: Inner };

        pub const main := fn(): i32 {
            var o: Outer = undefined;
            o.lead = 7i64;
            o.pad  = 9;
            o.inner.a = 20;
            o.inner.b = 22;

            const ip: ^mut Inner = ^mut o.inner;
            const op: ^mut Outer = @fieldParentPtr(Outer, "inner", ip);
            return op.inner.a + op.inner.b;
        };
    )") == 42);
}

} // namespace ghoti::tests
