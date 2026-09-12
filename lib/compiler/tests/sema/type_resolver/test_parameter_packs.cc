#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("a parameter pack function declares without error") {
    helpers::resolve_and_check("const f := fn(rest...): void {};");
    helpers::resolve_and_check(R"(
        const Format := interface {};
        const f := fn(rest: impl Format...): void {};
    )");
}

TEST_CASE("a parameter pack cannot appear in a fn(...): type value expression") {
    CHECK(helpers::raised("const F := fn(rest...): void;", sema::error::MALFORMED_PACK_USE));
}

TEST_CASE("a call to a pack function accepts any number of trailing arguments") {
    helpers::resolve_and_check(R"(
        const f := fn(a: i32, rest...): void {};
        const use := fn(): void { f(1); f(1, 2); f(1, 2, 3); };
    )");
    CHECK(helpers::raised(R"(
        const f := fn(a: i32, rest...): void {};
        const use := fn(): void { f(); };
    )",
                          sema::error::ARITY_MISMATCH));
}

TEST_CASE("`rest.len` and `rest[k]` resolve in a pack function's body") {
    helpers::resolve_and_check(R"(
        const f := fn(rest...): i32 { return @intCast(i32, rest.len); };
        const use := fn(): void { _ = f(1, 2, 3); };
    )");
    helpers::resolve_and_check(R"(
        const f := fn(rest...): i32 { return rest[0] + rest[1]; };
        const use := fn(): void { _ = f(1, 2); };
    )");
}

TEST_CASE("`rest[k]` requires an in-range, compile-time constant index") {
    CHECK(helpers::raised(R"(
        const f := fn(rest...): i32 { return rest[5]; };
        const use := fn(): void { _ = f(1, 2); };
    )",
                          sema::error::PACK_INDEX_OUT_OF_RANGE));
    CHECK(helpers::raised(R"(
        const f := fn(rest...): i32 { var i: usize = 0; return rest[i]; };
        const use := fn(): void { _ = f(1, 2); };
    )",
                          sema::error::PACK_INDEX_NOT_CONST));
}

TEST_CASE("an `impl I...` bound is enforced against every pack argument") {
    helpers::resolve_and_check(R"(
        const Format := interface { const fmt := fn(&self): i32; };
        const Good := struct {};
        impl Format for Good { const fmt := fn(&self): i32 { return 1; }; };
        const f := fn(rest: impl Format...): void {};
        const use := fn(): void { const g := Good{}; f(g, g, g); };
    )");
    CHECK(helpers::raised(R"(
        const Format := interface { const fmt := fn(&self): i32; };
        const Good := struct {};
        impl Format for Good { const fmt := fn(&self): i32 { return 1; }; };
        const f := fn(rest: impl Format...): void {};
        const use := fn(): void { const g := Good{}; f(g, 5); };
    )",
                          sema::error::UNSATISFIED_BOUND));
}

TEST_CASE("a bare parameter pack is out of position") {
    CHECK(helpers::raised(R"(
        const f := fn(rest...): void { var v := rest; };
        const use := fn(): void { f(1, 2); };
    )",
                          sema::error::PACK_USE_OUT_OF_POSITION));
}

TEST_CASE("`expr...` forwards a pack into another call's argument list") {
    helpers::resolve_and_check(R"(
        const g := fn(rest...): void {};
        const f := fn(rest...): void { g(rest...); };
        const use := fn(): void { f(1, 2, 3); };
    )");
    helpers::resolve_and_check(R"(
        const g := fn(a: i32, b: i32): void {};
        const f := fn(rest...): void { g(rest...); };
        const use := fn(): void { f(1, 2); };
    )");
}

TEST_CASE("`expr...` only expands the enclosing parameter pack") {
    CHECK(helpers::raised(R"(
        const g := fn(rest...): void {};
        const f := fn(rest...): void {
            var arr: [2]i32 = .{1, 2};
            g(arr...);
        };
        const use := fn(): void { f(1, 2); };
    )",
                          sema::error::PACK_EXPANSION_MISPLACED));
}

TEST_CASE("a spliced pack expansion's effective arity is checked against the callee") {
    CHECK(helpers::raised(R"(
        const g := fn(a: i32, b: i32): void {};
        const f := fn(rest...): void { g(rest...); };
        const use := fn(): void { f(1, 2, 3); };
    )",
                          sema::error::ARITY_MISMATCH));
}

} // namespace ghoti::tests
