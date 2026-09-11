#include <algorithm>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

// A pack parameter makes its function implicitly generic (like `auto`/`constexpr` params) - the
// declaration alone resolves fine; call-site behavior is covered in the e2e tests.
TEST_CASE("a parameter pack function declares without error") {
    helpers::resolve_and_check("const f := fn(rest...): void {};");
    helpers::resolve_and_check(R"(
        const Format := interface {};
        const f := fn(rest: impl Format...): void {};
    )");
}

// A pack function is not first-class (§8.4); it cannot denote a `fn(...): type` value.
TEST_CASE("a parameter pack cannot appear in a fn(...): type value expression") {
    CHECK(helpers::raised("const F := fn(rest...): void;",
                          sema::error::PACK_PARAM_NOT_YET_SUPPORTED));
}

// A pack accepts any number of trailing arguments, at or above the fixed-parameter count: none
// of these calls hit `ARITY_MISMATCH` (they still hit `PACK_PARAM_NOT_YET_SUPPORTED`, below).
TEST_CASE("a call to a pack function accepts any number of trailing arguments") {
    const auto codes{helpers::resolver_error_codes(R"(
        const f := fn(a: i32, rest...): void {};
        const use := fn(): void { f(1); f(1, 2); f(1, 2, 3); };
    )")};
    CHECK(!std::ranges::contains(codes, sema::error::ARITY_MISMATCH));

    CHECK(helpers::raised(R"(
        const f := fn(a: i32, rest...): void {};
        const use := fn(): void { f(); };
    )",
                          sema::error::ARITY_MISMATCH));
}

// Full call-site semantics (`rest.len`/`rest[K]`/monomorphization) land in a later phase.
TEST_CASE("calling a parameter pack function is not yet implemented") {
    CHECK(helpers::raised(R"(
        const f := fn(rest...): void {};
        const use := fn(): void { f(1, 2); };
    )",
                          sema::error::PACK_PARAM_NOT_YET_SUPPORTED));
}

} // namespace ghoti::tests
