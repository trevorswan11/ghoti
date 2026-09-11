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

} // namespace ghoti::tests
