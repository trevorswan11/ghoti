#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("A method calls a sibling &self method through the `self.m()` sugar") {
    CHECK(helpers::compile_and_run(R"(
        const File := struct {
            handle: i32,
            pub const write := fn(&self, n: i32): i32 {
                return self.handle + n;
            };
            pub const writeAll := fn(&self, n: i32): i32 {
                return self.write(n);
            };
        };
        pub const main := fn(): i32 {
            var f := File{ .handle = 10 };
            return f.writeAll(5);
        };
    )") == 15);
}

TEST_CASE("A `&mut self` method calls a sibling `&mut self` method via `self.m()`") {
    CHECK(helpers::compile_and_run(R"(
        const Counter := struct {
            n: i32,
            pub const bump := fn(&mut self): void {
                self.n = self.n + 1;
            };
            pub const bump3 := fn(&mut self): void {
                self.bump();
                self.bump();
                self.bump();
            };
        };
        pub const main := fn(): i32 {
            var c := Counter{ .n = 0 };
            c.bump3();
            return c.n;
        };
    )") == 3);
}

TEST_CASE("A method calls a method on a `^T` field via `self.field.m()`") {
    CHECK(helpers::compile_and_run(R"(
        const Inner := struct {
            base: i32,
            pub const get := fn(&self, k: i32): i32 {
                return self.base + k;
            };
        };
        const Outer := struct {
            inner: ^Inner,
            pub const go := fn(&self, k: i32): i32 {
                return self.inner.get(k);
            };
        };
        pub const main := fn(): i32 {
            var i := Inner{ .base = 100 };
            var o := Outer{ .inner = ^i };
            return o.go(7);
        };
    )") == 107);
}

TEST_CASE("A method calls a method on a `&T` field via `self.field.m()`") {
    CHECK(helpers::compile_and_run(R"(
        const Inner := struct {
            base: i32,
            pub const get := fn(&self): i32 {
                return self.base;
            };
        };
        const Outer := struct {
            inner: &Inner,
            pub const go := fn(&self): i32 {
                return self.inner.get();
            };
        };
        pub const main := fn(): i32 {
            var i := Inner{ .base = 42 };
            const o := Outer{ .inner = &i };
            return o.go();
        };
    )") == 42);
}

} // namespace ghoti::tests
