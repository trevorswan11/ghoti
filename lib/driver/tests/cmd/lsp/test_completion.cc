#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <stdx/memory.hh>

#include "compiler/module/overlay_loader.hh"
#include "driver/cmd/lsp/completion.hh"
#include "driver/cmd/lsp/rpc.hh"
#include "driver/cmd/lsp/session.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("completion_items lists keywords and top-level declarations") {
    constexpr std::string_view source{
        "pub const answer := 42;\n"
        "pub const add := fn(a: i32, b: i32): i32 { return a + b; };\n"
        "pub const point := struct { x: i32, y: i32 };\n"};

    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_completion.gh"};
    CHECK(loader.add(path, std::string{source}));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    const auto items = lsp::completion_items(*module);
    CHECK(lsp::has_field(items, "label", "const"));  // keyword
    CHECK(lsp::has_field(items, "label", "fn"));     // keyword
    CHECK(lsp::has_field(items, "label", "answer")); // top-level constant
    CHECK(lsp::has_field(items, "label", "add"));    // top-level function
    CHECK(lsp::has_field(items, "label", "point"));  // top-level struct
}

TEST_CASE("completion_items still surfaces valid top-level declarations around a syntax error") {
    constexpr std::string_view source{"pub const before := 1;\n"
                                      "pub const broken := before.;\n"
                                      "pub const after := 2;\n"};

    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_completion_broken.gh"};
    CHECK(loader.add(path, std::string{source}));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};
    // The LSP's analysis_session tolerates the syntax error and still runs sema over
    CHECK_FALSE(module->is_errored());
    CHECK_FALSE(module->parse_diagnostics.empty());

    const auto items = lsp::completion_items(*module);
    CHECK(lsp::has_field(items, "label", "before"));
    CHECK(lsp::has_field(items, "label", "after"));
    CHECK_FALSE(lsp::has_field(items, "label", "broken"));
}

} // namespace ghoti::tests
