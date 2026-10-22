#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("saturating binary '+|' rejects float operands") {
    helpers::test_resolver_fail(
        "const c = 1.0f32 +| 2.0f32;",
        sema::diagnostic{"operator '+|' expects two integer operands; found 'f32' and 'f32'",
                         sema::error::OPERATOR_TYPE_MISMATCH,
                         std::pair{0UZ, 10UZ}});
}

TEST_CASE("saturating binary '<<|' rejects pointer operands") {
    helpers::test_resolver_fail(
        "let mut p: ^i32 = undefined; const x = p <<| 1;",
        sema::diagnostic{"operator '<<|' expects two integer operands; found '^i32' and "
                         "'comptime_int'",
                         sema::error::OPERATOR_TYPE_MISMATCH,
                         std::pair{0UZ, 39UZ}});
}

TEST_CASE("saturating compound '*|=' rejects boolean operands") {
    helpers::test_resolver_fail(
        "let mut b: bool = true; b *|= true;",
        sema::diagnostic{"operator '*|=' expects two integer operands; found 'bool' and 'bool'",
                         sema::error::OPERATOR_TYPE_MISMATCH,
                         std::pair{0UZ, 24UZ}});
}

} // namespace ghoti::tests
