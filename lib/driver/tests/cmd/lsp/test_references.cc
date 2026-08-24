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

TEST_CASE("definition_span_at resolves the same span from a reference and the declaration name") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_references.gh"};
    CHECK(loader.add(path, std::string{source}));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    // Line 1, column 15 lands on the first `x` reference
    const auto from_reference{UNWRAP(lsp::definition_span_at(*module, {1, 15}))};
    // Line 0, column 10 lands on the `x` declaration name itself
    const auto from_declaration{UNWRAP(lsp::definition_span_at(*module, {0, 10}))};

    CHECK(from_reference.start.line == 0);
    CHECK(from_reference.start.column == 10);
    CHECK(from_reference.end.line == 0);
    CHECK(from_reference.end.column == 11);
    CHECK(from_reference == from_declaration);
}

TEST_CASE("find_references returns every reference to a definition, not the declaration itself") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_references_multi.gh"};
    CHECK(loader.add(path, std::string{source}));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    const auto definition{UNWRAP(lsp::definition_span_at(*module, {0, 10}))};
    const auto refs{lsp::find_references(*module, definition)};

    REQUIRE(refs.size() == 2);
    CHECK(refs[0].start.line == 1);
    CHECK(refs[0].start.column == 15);
    CHECK(refs[1].start.line == 1);
    CHECK(refs[1].start.column == 19);
}

TEST_CASE("definition_span_at returns none off any identifier") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_references_none.gh"};
    CHECK(loader.add(path, std::string{source}));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    CHECK_FALSE(lsp::definition_span_at(*module, {0, 0})); // `pub`, not an identifier
}

} // namespace ghoti::tests
