#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("deferred @compileError: an unreferenced declaration is inert") {
    CHECK(helpers::resolve_diags(R"(
        pub const BROKEN := @compileError("port this C macro by hand");

        pub const main := fn(): i32 { return 0; };
    )")
              .codes.empty());
}

TEST_CASE("deferred @compileError: referencing the declaration reports its message") {
    const auto res{helpers::resolve_diags(R"(
        const BROKEN := @compileError("port this C macro by hand");

        pub const main := fn(): i32 { return BROKEN; };
    )")};
    REQUIRE_FALSE(res.codes.empty());
    CHECK(res.codes.front() == sema::error::COMPILE_ERROR_REACHED);
    CHECK(res.message_contains("port this C macro by hand"));
}

TEST_CASE("deferred @compileError: the message surfaces at the use site, not the declaration") {
    const auto res{helpers::resolve_diags(R"(
        const A := @compileError("first");
        const B := @compileError("second");

        pub const main := fn(): i32 { return B; };
    )")};
    REQUIRE_FALSE(res.codes.empty());
    CHECK(res.message_contains("second"));
    CHECK_FALSE(res.message_contains("first"));
}

TEST_CASE("deferred @compileError: only fires for the whole-initializer form") {
    const auto res{helpers::resolve_diags(R"(
        const X := 1 + @compileError("still eager");

        pub const main := fn(): i32 { return 0; };
    )")};
    REQUIRE_FALSE(res.codes.empty());
    CHECK(res.message_contains("still eager"));
}

} // namespace ghoti::tests
