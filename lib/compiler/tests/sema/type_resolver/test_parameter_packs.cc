#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

// Full semantics land in a later phase; for now a pack function must fail cleanly, not crash.
TEST_CASE("a parameter pack function is not yet resolvable") {
    CHECK(helpers::raised("const f := fn(rest...): void {};",
                          sema::error::PACK_PARAM_NOT_YET_SUPPORTED));
    CHECK(helpers::raised(R"(
        const Format := interface {};
        const f := fn(rest: impl Format...): void {};
    )",
                          sema::error::PACK_PARAM_NOT_YET_SUPPORTED));
}

} // namespace ghoti::tests
