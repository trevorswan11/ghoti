#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`constexpr var` is a legal mutable comptime local") {
    helpers::resolve_and_check("constexpr var n := 0;");
    helpers::resolve_and_check("constexpr var n: i32 = 0;");
}

TEST_CASE("`&`/`^` on a `constexpr var` is a compile error") {
    CHECK(helpers::raised("const use := fn(): void { constexpr var n := 0; const p := &n; };",
                          sema::error::CONSTEXPR_VAR_ADDRESS_OF));
    CHECK(helpers::raised("const use := fn(): void { constexpr var n := 0; const p := ^n; };",
                          sema::error::CONSTEXPR_VAR_ADDRESS_OF));
}

} // namespace ghoti::tests
