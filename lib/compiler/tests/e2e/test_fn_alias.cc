#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("calling a module-scope function alias directly") {
    CHECK(helpers::compile_and_run(R"(
        const inc := fn(n: i32): i32 { return n + 1; };
        const bump := inc;

        pub const main := fn(): i32 {
            return bump(41);
        };
    )") == 42);
}

TEST_CASE("passing a module-scope function alias as a `fn`-pointer argument") {
    CHECK(helpers::compile_and_run(R"(
        const dbl := fn(n: i32): i32 { return n * 2; };
        const twice := dbl;

        const apply := fn(f: fn(i32): i32, x: i32): i32 { return f(x); };

        pub const main := fn(): i32 {
            return apply(twice, 21);
        };
    )") == 42);
}

TEST_CASE("an alias of an alias") {
    CHECK(helpers::compile_and_run(R"(
        const inc := fn(n: i32): i32 { return n + 1; };
        const a := inc;
        const b := a;

        pub const main := fn(): i32 {
            return b(41);
        };
    )") == 42);
}

TEST_CASE("module-scope alias of a static member function") {
    CHECK(helpers::compile_and_run(R"(
        const Box := struct {
            n: i32,
            const of := fn(v: i32): @this() { return .{ .n = v }; };
        };

        const make := Box.of;
        const relay := fn(f: fn(i32): Box, x: i32): Box { return f(x); };

        pub const main := fn(): i32 {
            const a := make(40);        // direct call through the alias
            const b := relay(make, 2);  // alias passed as a fn-pointer argument
            return a.n + b.n;
        };
    )") == 42);
}

TEST_CASE("unbound method reference: `^self` and `&mut self` receivers") {
    CHECK(helpers::compile_and_run(R"(
        const Box := struct {
            n: i32,
            const of := fn(v: i32): @this() { return .{ .n = v }; };
            const scaled := fn(^self, k: i32): i32 { return self.n * k; };
            const set := fn(&mut self, v: i32): void { self.n = v; };
        };

        pub const main := fn(): i32 {
            var b := Box.of(3);
            const scale := Box.scaled;   // fn(^Box, i32): i32
            const store := Box.set;      // fn(&mut Box, i32): void
            store(&mut b, 6);
            return scale(^b, 7);
        };
    )") == 42);
}

TEST_CASE("unbound method reference passed as a `fn`-pointer argument") {
    CHECK(helpers::compile_and_run(R"(
        const Box := struct {
            n: i32,
            const get := fn(&self): i32 { return self.n; };
        };

        const relay := fn(f: fn(&Box): i32, r: &Box): i32 { return f(r); };

        pub const main := fn(): i32 {
            const b := Box{ .n = 42 };
            return relay(Box.get, &b);
        };
    )") == 42);
}

TEST_CASE("a bodyless `fn(...)` type expression names a callable type") {
    CHECK(helpers::compile_and_run(R"(
        const BinOp := fn(a: i32, b: i32): i32;

        const apply := fn(f: BinOp, a: i32, b: i32): i32 { return f(a, b); };
        const add := fn(a: i32, b: i32): i32 { return a + b; };

        pub const main := fn(): i32 {
            return apply(add, 40, 2);
        };
    )") == 42);
}

TEST_CASE("an extracted method is not callable via a dot expression on an instance") {
    helpers::expect_compile_error(R"(
        const Box := struct {
            n: i32,
            const scaled := fn(^self, k: i32): i32 { return self.n * k; };
        };

        pub const main := fn(): i32 {
            const m := Box.scaled;
            const b := Box{ .n = 6 };
            return b.m(7);
        };
    )");
}

} // namespace ghoti::tests
