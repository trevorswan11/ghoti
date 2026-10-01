#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`comptime let mut` is a legal mutable comptime local") {
    helpers::resolve_and_check("comptime let mut n = 0;");
    helpers::resolve_and_check("comptime let mut n: i32 = 0;");
}

TEST_CASE("`&`/`^` on a `comptime let mut` is a compile error") {
    CHECK(helpers::raised("const use = fn(): void { comptime let mut n = 0; let p = &n; };",
                          sema::error::COMPTIME_MUT_ADDRESS_OF));
    CHECK(helpers::raised("const use = fn(): void { comptime let mut n = 0; let p = ^n; };",
                          sema::error::COMPTIME_MUT_ADDRESS_OF));
}

} // namespace ghoti::tests
