#include <filesystem>
#include <iostream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <stdx/memory.hh>

#include "compiler/module/overlay_loader.hh"
#include "driver/cmd/lsp/diagnostics.hh"
#include "driver/cmd/lsp/session.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("to_lsp_diagnostics is empty for a clean module") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_diagnostics_clean.gh"};
    CHECK(loader.add(path, "pub const x := 5;\n"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};
    CHECK(lsp::to_lsp_diagnostics(*module).empty());
}

TEST_CASE("to_lsp_diagnostics reports a syntax error with a code, message, and range") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_diagnostics_broken.gh"};
    CHECK(loader.add(path, "pub const x := ;\n"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    const auto diagnostics = lsp::to_lsp_diagnostics(*module);
    REQUIRE(diagnostics.size() == 1);

    const auto& diag{diagnostics.at(0)};
    CHECK(diag.at("severity") == 1);
    CHECK(diag.at("source") == "ghoti");
    CHECK(diag.at("code") == "MISSING_PREFIX_PARSER");
    CHECK_FALSE(diag.at("message").get<std::string>().empty());
    CHECK(diag.at("range").at("start").at("line") == 0);
}

} // namespace ghoti::tests
