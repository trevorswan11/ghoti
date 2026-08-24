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
        {"range",
         {{"start", {{"line", 0}, {"character", 5}}}, {"end", {{"line", 0}, {"character", 6}}}}}};
}

} // namespace

TEST_CASE("code_actions offers a missing-semicolon quick fix") {
    const nlohmann::json diagnostics{
        point_diagnostic("UNEXPECTED_TOKEN", "Expected token SEMICOLON, found PUBLIC")};

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

TEST_CASE("code_actions offers a missing-mutability-modifier quick fix") {
    const nlohmann::json diagnostics{point_diagnostic(
        "ILLEGAL_DECL_MODIFIERS", "Exactly one mutability modifier may be used; found 0")};

    const auto actions = lsp::code_actions("file:///test.gh", diagnostics);
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].at("title") == "Add 'const' modifier");

    const auto& edits = actions[0].at("edit").at("changes").at("file:///test.gh");
    CHECK(edits[0].at("newText") == "const ");
}

TEST_CASE("code_actions skips diagnostics it has no known fix for") {
    const nlohmann::json diagnostics{
        point_diagnostic("ILLEGAL_DECL_MODIFIERS",
                         "Exactly one mutability modifier may be used; found 2"),
        point_diagnostic("SOME_OTHER_ERROR", "unrelated message")};

    CHECK(lsp::code_actions("file:///test.gh", diagnostics).empty());
}

TEST_CASE("code_actions handles several diagnostics at once, only fixing the known ones") {
    const nlohmann::json diagnostics{
        point_diagnostic("UNEXPECTED_TOKEN", "Expected token SEMICOLON, found RBRACE"),
        point_diagnostic("UNEXPECTED_TOKEN", "Expected token RBRACE, found END"),
    };

    const auto actions = lsp::code_actions("file:///test.gh", diagnostics);
    CHECK(actions.size() == 1);
}

} // namespace ghoti::tests
