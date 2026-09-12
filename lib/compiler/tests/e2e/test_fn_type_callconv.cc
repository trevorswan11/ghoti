#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

// `fn(...): T` type-annotation syntax used to have no `callconv(...)` spelling of its own, so it
// always denoted the default (`.c`) convention regardless of what a value assigned to it actually
// imprinted - now it accepts the same `callconv(.x)` spelling an ordinary function declaration's
// signature already does, in the same position. Not calling through the resulting pointer here -
// whether a non-native calling convention is actually invokable correctly on this host target is
// a separate, deeper ABI/codegen question this doesn't chase; only the syntax/type-identity/
// assignability side is this fix's scope (the matching-`.c`-and-actually-calling case is already
// covered below).
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

// Known limitation, pre-existing and not fixed here: now that a `fn(...): T` variable's own
// callconv is real, provably-distinct type identity, this SHOULD be rejected the same way any
// other type mismatch is - but a function value's assignment doesn't route through
// `is_assignable`/`is_same_unqualified` at all (it decays through a separate, symbol-name-based
// path), so a mismatched callconv silently slips through today. Fixing that is a separate, deeper
// change to how a function value is coerced on assignment, not something this syntax addition
// itself needed to touch.
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
