#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdx/memory.hh>
#include <stdx/types.hh>

#include "compiler/module/overlay_loader.hh"
#include "driver/cmd/lsp/document_symbols.hh"
#include "driver/cmd/lsp/session.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view source{"pub const X = 5;\n"
                                  "let mut y = 1;\n"
                                  "const add = fn(a: i32, b: i32): i32 { return a + b; };\n"
                                  "const Point = struct { x: i32, py: i32 };\n"
                                  "const Color = enum { RED, GREEN, BLUE };\n"};

} // namespace

TEST_CASE("document_symbols outlines every top-level declaration with the right kind") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_document_symbols.gh"};
    CHECK(loader.add(path, std::string{source}));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    const auto symbols = lsp::document_symbols(*module);
    REQUIRE(symbols.size() == 5);

    CHECK(symbols.at(0).at("name") == "X");
    CHECK(symbols.at(0).at("kind") == 14); // Constant

    CHECK(symbols.at(1).at("name") == "y");
    CHECK(symbols.at(1).at("kind") == 13); // Variable

    CHECK(symbols.at(2).at("name") == "add");
    CHECK(symbols.at(2).at("kind") == 12); // Function

    CHECK(symbols.at(3).at("name") == "Point");
    CHECK(symbols.at(3).at("kind") == 23); // Struct

    CHECK(symbols.at(4).at("name") == "Color");
    CHECK(symbols.at(4).at("kind") == 10); // Enum
}

TEST_CASE("document_symbols lists what a destructuring binds, not its hidden value") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_document_symbols_destructure.gh"};
    CHECK(loader.add(path,
                     "const P = struct { pub x: i32, pub y: i32 };\n"
                     "pub const .{ .x = |A|, .y = |B| } = P{ .x = 1, .y = 2 };\n"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    const auto symbols = lsp::document_symbols(*module);
    const auto names{[&] {
        std::vector<std::string> out;
        for (const auto& symbol : symbols) { out.emplace_back(symbol["name"].get<std::string>()); }
        return out;
    }()};
    CHECK(std::ranges::contains(names, "A"));
    CHECK(std::ranges::contains(names, "B"));
    CHECK_FALSE(std::ranges::any_of(
        names, [](const std::string& name) { return name.starts_with("__ghoti"); }));
}

TEST_CASE("document_symbols selectionRange lands on the name; range covers the whole statement") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_document_symbols_range.gh"};
    CHECK(loader.add(path, "pub const X = 5;\n"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    const auto symbols = lsp::document_symbols(*module);
    REQUIRE(symbols.size() == 1);

    const auto& selection{symbols.at(0).at("selectionRange")};
    CHECK(selection.at("start").at("character") == 10);
    CHECK(selection.at("end").at("character") == 11);

    // The full statement range starts at `pub` and extends past the name
    const auto& range{symbols.at(0).at("range")};
    CHECK(range.at("start").at("character") == 0);
    CHECK(range.at("end").at("character") > selection.at("end").at("character").get<usize>());
}

} // namespace ghoti::tests
