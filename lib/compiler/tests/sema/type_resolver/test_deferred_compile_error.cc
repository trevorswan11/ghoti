#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("deferred @compileError: an unreferenced declaration is inert") {
    CHECK(helpers::resolve_diags(R"(
        pub const BROKEN = @compileError("port this C macro by hand");

        pub const main = fn(): i32 { return 0; };
    )")
              .codes.empty());
}

TEST_CASE("deferred @compileError: referencing the declaration reports its message") {
    const auto res{helpers::resolve_diags(R"(
        const BROKEN = @compileError("port this C macro by hand");

        pub const main = fn(): i32 { return BROKEN; };
    )")};
    REQUIRE_FALSE(res.codes.empty());
    CHECK(res.codes.front() == sema::error::COMPILE_ERROR_REACHED);
    CHECK(res.message_contains("port this C macro by hand"));
}

TEST_CASE("deferred @compileError: the message surfaces at the use site, not the declaration") {
    const auto res{helpers::resolve_diags(R"(
        const A = @compileError("first");
        const B = @compileError("second");

        pub const main = fn(): i32 { return B; };
    )")};
    REQUIRE_FALSE(res.codes.empty());
    CHECK(res.message_contains("second"));
    CHECK_FALSE(res.message_contains("first"));
}

TEST_CASE("deferred @compileError: only fires for the whole-initializer form") {
    const auto res{helpers::resolve_diags(R"(
        const X = 1 + @compileError("still eager");

        pub const main = fn(): i32 { return 0; };
    )")};
    REQUIRE_FALSE(res.codes.empty());
    CHECK(res.message_contains("still eager"));
}

TEST_CASE("@compileError: a message built with `++` is reported in full") {
    const auto res{helpers::resolve_diags(R"(
        pub const main = fn(): i32 {
            comptime { @compileError("expected an int, found " ++ @typeName(bool)); }
            return 0;
        };
    )")};
    REQUIRE_FALSE(res.codes.empty());
    CHECK(res.message_contains("expected an int, found bool"));
}

TEST_CASE("@compileError: a generic's error also points at the call that instantiated it") {
    const auto res{helpers::resolve_diags(R"(
        const only_bool = fn(val: auto): bool {
            const V = @TypeOf(val);
            if comptime (V == bool) { return val; } else @compileError("not a bool: " ++ @typeName(V));
        };
        pub const main = fn(): i32 {
            _ = only_bool(1.5);
            return 0;
        };
    )")};
    CHECK(std::ranges::contains(res.codes, sema::error::COMPILE_ERROR_REACHED));
    CHECK(res.message_contains("not a bool: f64"));
    CHECK(res.message_contains("Failed to instantiate 'only_bool'"));
}

} // namespace ghoti::tests
