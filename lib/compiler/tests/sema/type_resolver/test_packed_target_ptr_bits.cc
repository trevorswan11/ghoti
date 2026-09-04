#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("packed struct pointer field sizing uses the real 32-bit target pointer width") {
    auto [ctx, idx]{
        helpers::resolve_for_target("const S := packed struct { p1: ^u8, p2: ^u8, tail: u1 };",
                                    "armv7-unknown-linux-gnueabihf")};
    helpers::check_errors<sema::diagnostics>(ctx->root_mod);
}

TEST_CASE("packed struct pointer field sizing still rejects an over-wide struct on a 64-bit "
          "target") {
    auto [ctx, idx]{helpers::resolve_for_target(
        "const S := packed struct { p1: ^u8, p2: ^u8, tail: u1 };", "x86_64-unknown-linux-gnu")};
    const auto& diags{UNWRAP(ctx->root_mod.diagnostics.as_opt<sema::diagnostics>())};
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].get_error() == sema::error::ILLEGAL_PACKED_FIELD);
}

} // namespace ghoti::tests
