#include <ranges>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Test statement symbol collection") {
    auto [ctx, idx]{helpers::collect_and_check(R"(test "foo" { let foo = bar; })")};
    const auto& registry{ctx->analyzer.get_registry()};
    REQUIRE(registry.size() == 2);
    const auto& table{UNWRAP(registry.get_opt(idx))};
    CHECK(table.size() == 0);

    const auto  first_node{ctx->root_mod.ast | std::views::take(1)};
    const auto& test_type{UNWRAP(ctx->root_mod.get_sema_type_opt(*first_node.begin()))};
    CHECK(test_type == ctx->get_type(sema::type_kind::BLOCK, 1));
    ctx->test_common_decl_collection(1);
}

TEST_CASE("Test shadowing") {
    helpers::test_collector_fail(
        R"(const a = 2; test "foo" { let a = 3; })",
        sema::diagnostic{"Attempt to shadow identifier 'a'; previous declaration here: 1:7",
                         sema::error::SHADOWING_DECLARATION,
                         std::pair{0UZ, 30UZ}});
}

TEST_CASE("Illegal test location") {
    helpers::test_collector_fail("const a = fn(&self): void { test {} };",
                                 sema::diagnostic{"Tests must be at the topmost level of a file",
                                                  sema::error::ILLEGAL_TEST_LOCATION,
                                                  std::pair{0UZ, 28UZ}});
}

TEST_CASE("Test-only builtins are rejected outside a test block") {
    helpers::test_collector_fail(
        "const a = fn(): void { @expect(true); };",
        sema::diagnostic{
            "'@expect' may only be used inside a 'test' block or a `@[testing]` function",
            sema::error::TEST_BUILTIN_OUTSIDE_TEST,
            std::pair{0UZ, 23UZ}});
    helpers::test_collector_fail(
        "const a = fn(): void { @require(true); };",
        sema::diagnostic{
            "'@require' may only be used inside a 'test' block or a `@[testing]` function",
            sema::error::TEST_BUILTIN_OUTSIDE_TEST,
            std::pair{0UZ, 23UZ}});
    helpers::test_collector_fail(
        "const a = fn(): void { @skip(\"x\"); };",
        sema::diagnostic{
            "'@skip' may only be used inside a 'test' block or a `@[testing]` function",
            sema::error::TEST_BUILTIN_OUTSIDE_TEST,
            std::pair{0UZ, 23UZ}});
}

TEST_CASE("Test-only builtins collect cleanly inside a test block") {
    helpers::collect_and_check(R"(test "t" { @expect(a == b); @require(c); @skip("later"); })");
}

TEST_CASE("A `@[testing]` function may use the test builtins, but a closure inside it may not") {
    CHECK_FALSE(helpers::raised(R"(
        @[testing]
        const helper = fn(): void {
            @expect(true);
            @require(true);
            @skip();
        };
    )",
                                sema::error::TEST_BUILTIN_OUTSIDE_TEST));
    CHECK(helpers::raised(R"(
        @[testing]
        const helper = fn(): void {
            let inner = fn(): void { @require(true); };
            inner();
        };
    )",
                          sema::error::TEST_BUILTIN_OUTSIDE_TEST));
}

TEST_CASE("A `@[testing]` function is only called directly from a test or another one") {
    constexpr auto HELPER{R"(
        @[testing]
        const helper = fn(): void { @expect(true); };
    )"};
    const auto     raised{[&](std::string_view rest) {
        return helpers::raised(fmt::format("{}{}", HELPER, rest),
                               sema::error::TESTING_FN_OUTSIDE_TEST);
    }};
    CHECK_FALSE(raised(R"(test { helper(); })"));
    CHECK_FALSE(raised(R"(@[testing] const outer = fn(): void { helper(); };)"));
    CHECK(raised(R"(const plain = fn(): void { helper(); };)"));
    CHECK(raised(R"(test { let f = helper; f(); })"));
    CHECK(raised(R"(test { let c = fn(): void { helper(); }; c(); })"));
}

} // namespace ghoti::tests
