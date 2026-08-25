#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdx/memory.hh>

#include "compiler/module/overlay_loader.hh"
#include "driver/cmd/lsp/references.hh"
#include "driver/cmd/lsp/session.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view source{"pub const x := 5;\n"
                                  "pub const y := x + x;\n"};

} // namespace

TEST_CASE(
    "definition_location_at resolves the same location from a reference and the declaration name") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_references.gh"};
    CHECK(loader.add(path, std::string{source}));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    // Line 1, column 15 lands on the first `x` reference
    const auto from_reference{UNWRAP(lsp::definition_location_at(*module, {1, 15}))};
    // Line 0, column 10 lands on the `x` declaration name itself
    const auto from_declaration{UNWRAP(lsp::definition_location_at(*module, {0, 10}))};

    CHECK(from_reference.span.start.line == 0);
    CHECK(from_reference.span.start.column == 10);
    CHECK(from_reference.span.end.line == 0);
    CHECK(from_reference.span.end.column == 11);
    CHECK(from_reference == from_declaration);
}

TEST_CASE("find_references returns every reference to a definition, not the declaration itself") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_references_multi.gh"};
    CHECK(loader.add(path, std::string{source}));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    const auto definition{UNWRAP(lsp::definition_location_at(*module, {0, 10}))};
    const auto refs{lsp::find_references(session->get_manager(), definition)};

    REQUIRE(refs.size() == 2);
    CHECK(refs[0].span.start.line == 1);
    CHECK(refs[0].span.start.column == 15);
    CHECK(refs[1].span.start.line == 1);
    CHECK(refs[1].span.start.column == 19);
}

TEST_CASE("definition_location_at resolves a cross-module `::` access into the imported file") {
    mod::overlay_loader         loader;
    const std::filesystem::path helper_path{"test_xmod_helper.gh"};
    const std::filesystem::path main_path{"test_xmod_main.gh"};
    CHECK(loader.add(helper_path, "pub const value := 42;\n"));
    CHECK(loader.add(main_path,
                     "import \"test_xmod_helper.gh\" as helper;\n"
                     "pub const x := helper::value;\n"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(main_path))};

    // Line 1, column 23 lands on `value` in `helper::value`
    const auto def{UNWRAP(lsp::definition_location_at(*module, {1, 23}))};
    CHECK(def.path == std::filesystem::weakly_canonical(helper_path));
    CHECK(def.span.start.line == 0);
    CHECK(def.span.start.column == 10);
    CHECK(def.span.end.column == 15);

    // find_references searches the whole manager
    const auto refs{lsp::find_references(session->get_manager(), def)};
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].path == std::filesystem::weakly_canonical(main_path));
    CHECK(refs[0].span.start.line == 1);
    CHECK(refs[0].span.start.column == 23);
}

TEST_CASE("definition_location_at resolves a struct field access in the same module") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_field_same_module.gh"};
    CHECK(loader.add(path,
                     "pub const point := struct { x: i32, y: i32 };\n"
                     "pub const p := point{ .x = 1, .y = 2 };\n"
                     "pub const px := p.x;\n"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    // Line 2, column 18 lands on the `x` in `p.x`
    const auto def{UNWRAP(lsp::definition_location_at(*module, {2, 18}))};
    CHECK(def.path == std::filesystem::weakly_canonical(path));
    CHECK(def.span.start.line == 0);
}

TEST_CASE("definition_location_at resolves a struct field access across an import") {
    mod::overlay_loader         loader;
    const std::filesystem::path helper_path{"test_field_xmod_helper.gh"};
    const std::filesystem::path main_path{"test_field_xmod_main.gh"};
    CHECK(loader.add(helper_path, "pub const point := struct { pub x: i32, pub y: i32 };\n"));
    CHECK(loader.add(main_path,
                     "import \"test_field_xmod_helper.gh\" as helper;\n"
                     "pub const p := helper::point{ .x = 1, .y = 2 };\n"
                     "pub const px := p.x;\n"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(main_path))};

    // Line 2, column 18 lands on the `x` in `p.x`
    const auto def{UNWRAP(lsp::definition_location_at(*module, {2, 18}))};
    CHECK(def.path == std::filesystem::weakly_canonical(helper_path));
    CHECK(def.span.start.line == 0);
}

TEST_CASE("analyze can be called twice on the same session for different entry paths") {
    // Mirrors what document_store's persistent-graph rebuild does
    mod::overlay_loader         loader;
    const std::filesystem::path helper_path{"test_reanalyze_helper.gh"};
    const std::filesystem::path main_path{"test_reanalyze_main.gh"};
    CHECK(loader.add(helper_path, "pub const value := 42;\n"));
    CHECK(loader.add(main_path,
                     "import \"test_reanalyze_helper.gh\" as helper;\n"
                     "pub const x := helper::value;\n"));

    auto session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    CHECK(session->analyze(main_path));                              // pulls helper in as an import
    const auto helper_module{UNWRAP(session->analyze(helper_path))}; // reanalyzed as its own entry
    CHECK_FALSE(helper_module->is_errored());

    const auto definition{UNWRAP(lsp::definition_location_at(*helper_module, {0, 10}))};
    const auto refs{lsp::find_references(session->get_manager(), definition)};
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].path == std::filesystem::weakly_canonical(main_path));
}

TEST_CASE("definition_location_at resolves a using-alias reference to just the alias") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_references_using_alias.gh"};
    CHECK(loader.add(path, "using X = i32;\npub const s := @sizeOf(X);\n"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    const auto def{UNWRAP(lsp::definition_location_at(*module, {1, 23}))};
    CHECK(def.span.start.line == 0);
    CHECK(def.span.start.column == 6);
    CHECK(def.span.end.line == 0);
    CHECK(def.span.end.column == 7);
}

TEST_CASE("definition_location_at resolves a using-alias from its own declaration name") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_references_using_decl.gh"};
    CHECK(loader.add(path, "using X = i32;\n"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    const auto def{UNWRAP(lsp::definition_location_at(*module, {0, 6}))};
    CHECK(def.span.start.column == 6);
    CHECK(def.span.end.column == 7);
}

TEST_CASE("definition_location_at returns none off any identifier") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_references_none.gh"};
    CHECK(loader.add(path, std::string{source}));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    CHECK_FALSE(lsp::definition_location_at(*module, {0, 0})); // `pub`, not an identifier
}

} // namespace ghoti::tests
