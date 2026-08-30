#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

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

} // namespace ghoti::tests
