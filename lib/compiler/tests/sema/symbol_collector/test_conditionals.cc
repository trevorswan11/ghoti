#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] auto to_table_size(const sema::symbol_table& table) -> usize { return table.size(); }

// Checks that the scope has the foo-bar decl
auto test_conditional_scope(helpers::sema_test_context& ctx, usize idx) {
    auto& registry{ctx.analyzer.get_registry()};
    CHECK(registry.get_opt(idx).transform(to_table_size) == 1);
    ctx.test_common_decl_collection(idx);
}

} // namespace

TEST_CASE("If expression collection") {
    auto [ctx, idx]{helpers::collect_and_check(
        "const a := if (b) { const foo := bar; } else { const foo := bar; };")};

    auto& analyzer{ctx->analyzer};
    CHECK(analyzer.get_registry().size() == 3);
    const auto& actual{UNWRAP(analyzer.get_table_opt(idx))};
    CHECK(actual.size() == 1);
    REQUIRE(actual.get_opt("a"));

    test_conditional_scope(*ctx, 1);
    test_conditional_scope(*ctx, 2);
}

TEST_CASE("Flat if collection") { helpers::collect_and_check("const a := if (b > 4) c; else d;"); }

TEST_CASE("Match expression collection") {
    auto [ctx, idx]{helpers::collect_and_check(
        "const a := match (b) { c => |d| { const foo := bar; }, _ => { const foo := bar; }, "
        "e => |_| { const foo := bar; } };")};

    auto& registry{ctx->analyzer.get_registry()};
    CHECK(registry.size() == 7);
    const auto& actual{UNWRAP(registry.get_opt(idx))};
    CHECK(actual.size() == 1);
    REQUIRE(actual.get_opt("a"));

    CHECK(registry.get_opt(1).transform(to_table_size) == 1);
    test_conditional_scope(*ctx, 2);
    CHECK(registry.get_opt(3).transform(to_table_size) == 0);
    test_conditional_scope(*ctx, 4);
    CHECK(registry.get_opt(5).transform(to_table_size) == 0);
    test_conditional_scope(*ctx, 6);
}

TEST_CASE("Flat match collection") {
    helpers::collect_and_check("const a := match (b) { c => |d| d, e => |_| f, g => h, _ => i };");
}

namespace {

[[nodiscard]] auto expected_diag(usize col) -> sema::diagnostic {
    return {"Attempt to shadow identifier 'a'; previous declaration here: 1:7",
            sema::error::SHADOWING_DECLARATION,
            std::pair{0UZ, col}};
}

} // namespace

TEST_CASE("If expression inner shadowing") {
    helpers::test_collector_fail("const a := if (b) { var a: i32; };", expected_diag(24));
    helpers::test_collector_fail("const a := if (b) { var c: i32; } else { var a: i32; };",
                                 expected_diag(45));
}

TEST_CASE("Match shadowing assignee") {
    helpers::test_collector_fail("const a := match (c) { b => |a| b, };", expected_diag(29));
    helpers::test_collector_fail("const a := match (c) { b => { var a: i32; } };",
                                 expected_diag(34));
    helpers::test_collector_fail("const a := match (b) { c => d, _ => { var a: i32; } };",
                                 expected_diag(42));
}

TEST_CASE("Match dispatch shadowing") {
    helpers::test_collector_fail(
        "const a := match (c) { b => |c| { var c: i32; } };",
        sema::diagnostic{"Attempt to shadow identifier 'c'; previous declaration here: 1:30",
                         sema::error::SHADOWING_DECLARATION,
                         std::pair{0UZ, 38UZ}});
}

namespace {

// Collects `src` and returns the first sema diagnostic code, if any.
[[nodiscard]] auto first_collect_error(std::string_view src) -> stdx::option<sema::error> {
    auto [ctx, idx]{helpers::collect(src)};
    if (const auto diags{ctx->root_mod.diagnostics.as_opt<sema::diagnostics>()};
        diags && !diags->empty()) {
        return (*diags)[0].get_error();
    }
    return stdx::none;
}

} // namespace

TEST_CASE("Control-flow constructs are rejected as bare top-level statements") {
    CHECK(first_collect_error("if (true) { const a := 1; };") ==
          sema::error::ILLEGAL_TOP_LEVEL_STATEMENT);
    CHECK(first_collect_error("if constexpr (true) { const a := 1; } else { const b := 2; };") ==
          sema::error::ILLEGAL_TOP_LEVEL_STATEMENT);
    CHECK(first_collect_error("match (1) { _ => 0 };") == sema::error::ILLEGAL_TOP_LEVEL_STATEMENT);
    CHECK(first_collect_error("while (true) { };") == sema::error::ILLEGAL_TOP_LEVEL_STATEMENT);
    CHECK(first_collect_error("for (0..3) |_| { };") == sema::error::ILLEGAL_TOP_LEVEL_STATEMENT);
    CHECK(first_collect_error("loop { };") == sema::error::ILLEGAL_TOP_LEVEL_STATEMENT);
    CHECK(first_collect_error("blk: { break :blk 1; };") ==
          sema::error::ILLEGAL_TOP_LEVEL_STATEMENT);
}

TEST_CASE("Binding control flow with a top-level `const` is still allowed") {
    CHECK_FALSE(first_collect_error("const a := if (true) 1 else 2;"));
    CHECK_FALSE(first_collect_error("const a := if constexpr (true) 1 else 2;"));
    CHECK_FALSE(first_collect_error("const a := match (1) { 1 => 10, _ => 20 };"));
    CHECK_FALSE(first_collect_error("const a := blk: { break :blk 7; };"));
    CHECK_FALSE(first_collect_error("@setMainSymbol(\"go\"); pub const go := fn(): void {};"));
}

} // namespace ghoti::tests
