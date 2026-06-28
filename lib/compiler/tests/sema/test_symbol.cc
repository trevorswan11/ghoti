#include <ranges>
#include <string>
#include <tuple>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/ast/handle.hh"
#include "compiler/ast/statement.hh"
#include "compiler/module/memory_loader.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] auto setup_basic_import() {
    mod::memory_loader loader;
    loader.add(helpers::TEST_FILENAME, std::string{"import a;"});
    mod::module_manager manager{loader};
    REQUIRE(manager.add_library_module("a", std::string{helpers::TEST_FILENAME}));
    const auto mod_result{manager.try_get_library_module("a")};
    const auto module{*mod_result};
    REQUIRE_FALSE(module->is_errored());
    const auto first_node{module->ast | std::views::take(1)};
    const auto import_handle{ast::import_handle{*first_node.begin()}};

    return std::tuple{module, import_handle, std::pair{std::move(loader), std::move(manager)}};
}

} // namespace

TEST_CASE("Basic table operations") {
    const auto [module, import_handle, memory]{setup_basic_import()};

    sema::symbol_table table;
    CHECK(table.empty());
    CHECK(table.insert<sema::symbols::node_t>("a", *module, import_handle));
    CHECK(table.size() == 1);
    CHECK_FALSE(table.empty());

    CHECK(table.has("a"));
    const auto& retrieved{helpers::unwrap(table.get_opt("a"))};
    CHECK(retrieved.get_name() == "a");

    const auto symbolic_node{helpers::unwrap(retrieved.get_data().as_opt<sema::symbols::node_t>())};
    CHECK(symbolic_node->is<ast::import_stmt>());
}

TEST_CASE("Duplicate table inserts") {
    const auto [module, import_handle, memory]{setup_basic_import()};
    sema::symbol_table table;
    CHECK(table.insert<sema::symbols::node_t>("a", *module, import_handle));
    CHECK_FALSE(table.insert<sema::symbols::node_t>("a", *module, import_handle));
    CHECK(table.size() == 1);
}

TEST_CASE("Illegal registry insert") {
    const auto [module, import_handle, memory]{setup_basic_import()};
    sema::symbol_table_registry registry;
    const auto                  actual{helpers::unwrap_err(
        registry.insert_into<sema::symbols::node_t>(0, *module, "a", import_handle))};
    CHECK(actual == sema::diagnostic{sema::error::INVALID_TABLE_IDX});
}

TEST_CASE("Safety checked registry operations") {
    sema::symbol_table_registry registry;
    CHECK_FALSE(registry.get_opt(1));
    CHECK_FALSE(registry.get_from_opt(1, "a"));
}

} // namespace ghoti::tests
