#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("`.sysv` is rejected on a non-x86_64 target") {
    auto [ctx, idx]{helpers::resolve_for_target("const f := fn() callconv(.sysv): void {};",
                                                "aarch64-unknown-linux-gnu")};
    const auto& diags{UNWRAP(ctx->root_mod.diagnostics.as_opt<sema::diagnostics>())};
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].get_error() == sema::error::TYPE_MISMATCH);
}

TEST_CASE("`.win64` is rejected on a non-x86_64 target") {
    auto [ctx, idx]{helpers::resolve_for_target("const f := fn() callconv(.win64): void {};",
                                                "aarch64-unknown-linux-gnu")};
    const auto& diags{UNWRAP(ctx->root_mod.diagnostics.as_opt<sema::diagnostics>())};
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].get_error() == sema::error::TYPE_MISMATCH);
}

TEST_CASE("`.aapcs` is rejected on a non-arm target") {
    auto [ctx, idx]{helpers::resolve_for_target("const f := fn() callconv(.aapcs): void {};",
                                                "x86_64-unknown-linux-gnu")};
    const auto& diags{UNWRAP(ctx->root_mod.diagnostics.as_opt<sema::diagnostics>())};
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].get_error() == sema::error::TYPE_MISMATCH);
}

TEST_CASE("`.sysv` is accepted on an x86_64 target") {
    auto [ctx, idx]{helpers::resolve_for_target("const f := fn() callconv(.sysv): void {};",
                                                "x86_64-unknown-linux-gnu")};
    helpers::check_errors<sema::diagnostics>(ctx->root_mod);
}

TEST_CASE("`.aapcs` is accepted on an arm target") {
    auto [ctx, idx]{helpers::resolve_for_target("const f := fn() callconv(.aapcs): void {};",
                                                "armv7-unknown-linux-gnueabihf")};
    helpers::check_errors<sema::diagnostics>(ctx->root_mod);
}

TEST_CASE("`.c` is accepted on any target") {
    auto [ctx, idx]{helpers::resolve_for_target("const f := fn() callconv(.c): void {};",
                                                "aarch64-unknown-linux-gnu")};
    helpers::check_errors<sema::diagnostics>(ctx->root_mod);
}

} // namespace ghoti::tests
