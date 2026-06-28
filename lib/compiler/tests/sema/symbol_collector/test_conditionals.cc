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
    const auto& actual{helpers::unwrap(analyzer.get_table_opt(idx))};
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
    const auto& actual{helpers::unwrap(registry.get_opt(idx))};
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
    return {"Attempt to shadow identifier 'a'; previous declaration here: 1:1",
            sema::error::SHADOWING_DECLARATION,
            std::pair{0UZ, col}};
}

} // namespace

TEST_CASE("If expression inner shadowing") {
    helpers::test_collector_fail("const a := if (b) { var a: i32; };", expected_diag(20));
    helpers::test_collector_fail("const a := if (b) { var c: i32; } else { var a: i32; };",
                                 expected_diag(41));
}

TEST_CASE("Match shadowing assignee") {
    helpers::test_collector_fail("const a := match (c) { b => |a| b, };", expected_diag(29));
    helpers::test_collector_fail("const a := match (c) { b => { var a: i32; } };",
                                 expected_diag(30));
    helpers::test_collector_fail("const a := match (b) { c => d, _ => { var a: i32; } };",
                                 expected_diag(38));
}

TEST_CASE("Match dispatch shadowing") {
    helpers::test_collector_fail(
        "const a := match (c) { b => |c| { var c: i32; } };",
        sema::diagnostic{"Attempt to shadow identifier 'c'; previous declaration here: 1:30",
                         sema::error::SHADOWING_DECLARATION,
                         std::pair{0UZ, 34UZ}});
}

} // namespace ghoti::tests
