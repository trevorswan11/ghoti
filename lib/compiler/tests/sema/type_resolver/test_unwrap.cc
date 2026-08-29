#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Resolving well-formed `?` propagation") {
    SECTION("`?` yields the `ok` payload, enclosing function returns a matching Result") {
        helpers::resolve_and_check(R"(
        const R := union { ok: i32, err: i32 };
        const g := fn(): R { return R{ .ok = 1 }; };
        const f := fn(): R { const v := g()?; return R{ .ok = v }; };
    )");
    }

    SECTION("`?` yields the `some` payload, enclosing function returns a matching Optional") {
        helpers::resolve_and_check(R"(
        const O := union { some: i32, none: void };
        const g := fn(): O { return O{ .some = 1 }; };
        const f := fn(): O { const v := g()?; return O{ .some = v }; };
    )");
    }

    SECTION("Structurally valid: same family, and the two `ok` payload types need not match") {
        helpers::resolve_and_check(R"(
        const E := union { ok: i32, err: i32 };
        const F := union { ok: bool, err: i32 };
        const inner := fn(): E { return E{ .ok = 1 }; };
        const outer := fn(): F { const v := inner()?; _ = v; return F{ .ok = true }; };
    )");
    }
}

TEST_CASE("Resolving well-formed `!` assert-unwrap") {
    helpers::resolve_and_check(R"(
        const R := union { ok: i32, err: bool };
        const f := fn(r: R): i32 { return r!; };
    )");

    helpers::resolve_and_check(R"(
        const O := union { some: i32, none: void };
        const f := fn(o: O): i32 { return o!; };
    )");
}

TEST_CASE("`?` / `!` on a non-Result operand is rejected") {
    helpers::test_resolver_fail(
        "const f := fn(x: i32): i32 { x?; };",
        sema::diagnostic{"the postfix '?' operator expects a tagged union shaped like "
                         "'union { ok: T, err: E }' or 'union { some: T, none: void }'; "
                         "its operand has type 'i32'",
                         sema::error::UNWRAP_ON_NON_RESULT,
                         std::pair{0UZ, 29UZ}});

    helpers::test_resolver_fail(
        "const f := fn(x: i32): i32 { x!; };",
        sema::diagnostic{"the postfix '!' operator expects a tagged union shaped like "
                         "'union { ok: T, err: E }' or 'union { some: T, none: void }'; "
                         "its operand has type 'i32'",
                         sema::error::UNWRAP_ON_NON_RESULT,
                         std::pair{0UZ, 29UZ}});
}

TEST_CASE("`?` outside a function is rejected") {
    helpers::test_resolver_fail(
        R"(
const R := union { ok: i32, err: i32 };
const r := R{ .ok = 1 };
const b := r?;
)",
        sema::diagnostic{"the '?' operator can only be used inside a function",
                         sema::error::UNWRAP_OUTSIDE_FUNCTION,
                         std::pair{3UZ, 11UZ}});
}

TEST_CASE("`?` requires the enclosing function to return a matching Result / Optional") {
    SECTION("Enclosing function does not return a union at all") {
        helpers::test_resolver_fail(
            R"(
const R := union { ok: i32, err: i32 };
const f := fn(r: R): i32 {
    return r?;
};
)",
            sema::diagnostic{
                "the '?' operator propagates a 'union' but the enclosing function returns 'i32', "
                "which is not a matching 'union { ok: _, err: E }'",
                sema::error::UNWRAP_RETURN_TYPE_MISMATCH,
                std::pair{3UZ, 11UZ}});
    }

    SECTION("Optional `?` inside a Result-returning function: family mismatch") {
        helpers::test_resolver_fail(
            R"(
const O := union { some: i32, none: void };
const R := union { ok: i32, err: i32 };
const f := fn(o: O): R {
    return o?;
};
)",
            sema::diagnostic{
                "the '?' operator propagates a 'union' but the enclosing function returns 'union', "
                "which is not a matching 'union { some: _, none: void }'",
                sema::error::UNWRAP_RETURN_TYPE_MISMATCH,
                std::pair{4UZ, 11UZ}});
    }
}

} // namespace ghoti::tests
