#include <ranges>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Test statement symbol collection") {
    auto [ctx, idx]{helpers::collect_and_check(R"(test "foo" { const foo := bar; })")};
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
        R"(const a := 2; test "foo" { const a := 3; })",
        sema::diagnostic{"Attempt to shadow identifier 'a'; previous declaration here: 1:7",
                         sema::error::SHADOWING_DECLARATION,
                         std::pair{0UZ, 33UZ}});
}

TEST_CASE("Illegal test location") {
    helpers::test_collector_fail("const a := fn(&self): void { test {} };",
                                 sema::diagnostic{"Tests must be at the topmost level of a file",
                                                  sema::error::ILLEGAL_TEST_LOCATION,
                                                  std::pair{0UZ, 29UZ}});
}

} // namespace ghoti::tests
