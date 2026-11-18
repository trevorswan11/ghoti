#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/gir/dumper.hh"
#include "compiler/sema/error.hh"
#include "helpers/uir_diff.hh"

namespace ghoti::tests {

namespace {

// The codes of what UIR Sema reported, none when it accepted the program or stopped early
[[nodiscard]] auto reported_codes(std::string_view source) -> std::vector<sema::error> {
    const auto  run{helpers::run_uir_sema(source)};
    const auto& out{*run->out};
    REQUIRE_FALSE(out.stopped_at);
    std::vector<sema::error> codes;
    for (const auto& d : out.diagnostics) { codes.emplace_back(d.get_error()); }
    return codes;
}

[[nodiscard]] auto raw_gir(std::string_view source) -> std::string {
    const auto  run{helpers::run_uir_sema(source)};
    const auto& out{*run->out};
    REQUIRE(out.succeeded());
    std::ostringstream text;
    gir::dumper{text}.dump(*out.gir);
    return text.str();
}

[[nodiscard]] auto contains(std::string_view haystack, std::string_view needle) -> bool {
    return haystack.find(needle) != std::string_view::npos;
}

} // namespace

TEST_CASE("A plain `if` with a known condition analyzes both arms") {
    SECTION("A type error in the arm that never runs is still reported") {
        const auto codes{reported_codes(
            "const f = fn(): i32 { if (true) { return 1; } else { return true; } };")};
        CHECK(codes == std::vector{sema::error::RETURN_TYPE_MISMATCH});
    }

    SECTION("Both arms are emitted, with the branch left for GIR to fold") {
        const auto gir{raw_gir("const f = fn(): i32 { if (false) { return 1; } return 2; };")};
        CHECK(contains(gir, "cond_goto false seg"));
        CHECK(contains(gir, "ret i32 1"));
        CHECK(contains(gir, "ret i32 2"));
    }
}

TEST_CASE("Declarations used during their own analysis are a cycle") {
    const auto codes{reported_codes("const a: i32 = b + 1;\nconst b: i32 = a;")};
    CHECK(codes == std::vector{sema::error::CYCLIC_DEPENDENCY});
}

TEST_CASE("A `const` local must be known at compile time") {
    const auto codes{reported_codes(
        "const f = fn(x: i32): i32 { let mut y: i32 = x; const c = y + 1; return c; };")};
    CHECK(codes == std::vector{sema::error::COMPTIME_MUT_NOT_FOLDABLE});
}

TEST_CASE("Untyped branch values take the type of their first use") {
    const auto gir{helpers::uir_gir("const f = fn(c: bool): i64 { return if (c) 1 else 2; };")};
    CHECK(contains(gir, "alloca i64"));
    CHECK(contains(gir, "store 1, %0"));
    CHECK(contains(gir, "store 2, %0"));
}

TEST_CASE("Comptime integers fold, typed runtime arithmetic is checked unless it wraps") {
    const auto gir{
        helpers::uir_gir("const f = fn(a: i32, b: i32): i32 { return (a + b) +% (2 * 3); };")};
    CHECK(contains(gir, "add.checked i32 param.0, param.1"));
    CHECK(contains(gir, "add i32 %0, 6"));
}

TEST_CASE("Operands without a common type are reported") {
    const auto codes{reported_codes("const f = fn(a: i32, b: u32): bool { return a < b; };")};
    CHECK(codes == std::vector{sema::error::NO_PEER_TYPE});
}

TEST_CASE("A construct this phase doesn't analyze stops the analysis") {
    const auto run{helpers::run_uir_sema("const P = struct { x: i32 };")};
    REQUIRE(run->out->stopped_at);
    CHECK_FALSE(run->out->succeeded());
}

} // namespace ghoti::tests
