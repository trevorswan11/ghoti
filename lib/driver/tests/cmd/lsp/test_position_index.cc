#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdx/memory.hh>

#include "compiler/module/overlay_loader.hh"
#include "driver/cmd/lsp/position_index.hh"
#include "driver/cmd/lsp/session.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view source{"pub const x := 5;\n"
                                  "pub const y := x + 1;\n"};

} // namespace

TEST_CASE("identifier_at finds a reference and its declaration through the side tables") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_position_index.gh"};
    CHECK(loader.add(path, std::string{source}));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    // Line 1 (0-indexed), column 15 lands on the `x` reference in `y := x + 1`
    const auto id{UNWRAP(lsp::identifier_at(*module, {1, 15}))};

    const auto& type{UNWRAP(module->get_sema_type_opt(id))};
    CHECK(type.to_string() == "i32");

    // get_symbol_span resolves to the declared name itself, not the whole `pub const` statement
    const auto def_span{UNWRAP(module->get_identifier_definition(id))};
    CHECK(def_span.start.line == 0);
    CHECK(def_span.start.column == 10);
    CHECK(def_span.end.line == 0);
    CHECK(def_span.end.column == 11);
}

TEST_CASE("identifier_at returns none off the end of an identifier and off any identifier") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_position_index_none.gh"};
    CHECK(loader.add(path, std::string{source}));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    CHECK_FALSE(lsp::identifier_at(*module, {1, 16})); // just past `x`
    CHECK_FALSE(lsp::identifier_at(*module, {0, 0}));  // `pub`, not an identifier_expr
}

} // namespace ghoti::tests
