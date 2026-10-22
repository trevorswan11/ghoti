#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "driver/cmd/lsp/code_actions.hh"

namespace ghoti::tests {

namespace {

auto point_diagnostic(std::string code, std::string message) -> nlohmann::json {
    return {
        {"code", std::move(code)},
        {"message", std::move(message)},
        {
            "range",
            {
                {"start", {{"line", 0}, {"character", 5}}},
                {
                    "end",
                    {
                        {"line", 0},
                        {"character", 6},
                    },
                },
            },
        },
    };
}

} // namespace

TEST_CASE("code_actions offers a missing-semicolon quick fix") {
    const nlohmann::json diagnostics{
        point_diagnostic("UNEXPECTED_TOKEN", "Expected ';', found 'pub'")};

    const auto actions = lsp::code_actions("file:///test.gh", diagnostics);
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].at("title") == "Insert missing ';'");
    CHECK(actions[0].at("kind") == "quickfix");

    const auto& edits = actions[0].at("edit").at("changes").at("file:///test.gh");
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].at("newText") == ";");
    // Zero-width insertion right at the diagnostic's reported start
    CHECK(edits[0].at("range").at("start") == edits[0].at("range").at("end"));
    CHECK(edits[0].at("range").at("start").at("character") == 5);
}

TEST_CASE("code_actions offers each binding form as a missing-binding quick fix") {
    const nlohmann::json diagnostics{
        point_diagnostic("ILLEGAL_DECL_MODIFIERS",
                         "Exactly one of 'const', 'let' or 'let mut' may be used; found 0")};

    const auto actions = lsp::code_actions("file:///test.gh", diagnostics);
    REQUIRE(actions.size() == 3);
    const auto new_text = [&](std::size_t i) {
        return actions[i].at("edit").at("changes").at("file:///test.gh")[0].at("newText");
    };
    CHECK(actions[0].at("title") == "Add 'const'");
    CHECK(new_text(0) == "const ");
    CHECK(actions[1].at("title") == "Add 'let'");
    CHECK(new_text(1) == "let ");
    CHECK(actions[2].at("title") == "Add 'let mut'");
    CHECK(new_text(2) == "let mut ");
}

TEST_CASE("code_actions offers quick fixes for other unambiguous missing tokens") {
    for (const auto* spelling : {"}", ")", "]", ":", ","}) {
        const nlohmann::json diagnostics{
            point_diagnostic("UNEXPECTED_TOKEN",
                             std::string{"Expected '"} + spelling + "', found the end of input")};

        const auto actions = lsp::code_actions("file:///test.gh", diagnostics);
        REQUIRE(actions.size() == 1);
        CHECK(actions[0].at("title") == std::string{"Insert missing '"} + spelling + "'");
        CHECK(actions[0].at("edit").at("changes").at("file:///test.gh")[0].at("newText") ==
              spelling);
    }
}

TEST_CASE("code_actions skips diagnostics it has no known fix for") {
    const nlohmann::json diagnostics{
        point_diagnostic("ILLEGAL_DECL_MODIFIERS",
                         "Exactly one of 'const', 'let' or 'let mut' may be used; found 2"),
        point_diagnostic("SOME_OTHER_ERROR", "unrelated message")};

    CHECK(lsp::code_actions("file:///test.gh", diagnostics).empty());
}

TEST_CASE("code_actions handles several diagnostics at once, only fixing the known ones") {
    const nlohmann::json diagnostics{
        point_diagnostic("UNEXPECTED_TOKEN", "Expected ';', found '}'"),
        point_diagnostic("UNEXPECTED_TOKEN", "Expected an identifier, found ';'"), // unsafe
    };

    const auto actions = lsp::code_actions("file:///test.gh", diagnostics);
    CHECK(actions.size() == 1);
}

} // namespace ghoti::tests
