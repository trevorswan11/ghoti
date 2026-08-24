#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "driver/cmd/lsp/text_edit.hh"
#include "support/diagnostic.hh"

namespace ghoti::tests {

TEST_CASE("offset_of finds byte offsets across lines") {
    constexpr std::string_view text{"abc\nde\nfghi"};
    CHECK(lsp::offset_of(text, {0, 0}) == 0);
    CHECK(lsp::offset_of(text, {0, 3}) == 3);  // end of line 0
    CHECK(lsp::offset_of(text, {1, 0}) == 4);  // start of line 1, past the '\n'
    CHECK(lsp::offset_of(text, {1, 2}) == 6);  // end of line 1
    CHECK(lsp::offset_of(text, {2, 4}) == 11); // end of the final, newline-less line
}

TEST_CASE("offset_of clamps out-of-range lines and columns to the text's end") {
    constexpr std::string_view text{"abc\nde"};
    CHECK(lsp::offset_of(text, {0, 100}) == 3); // column past line 0's end clamps to the '\n'
    CHECK(lsp::offset_of(text, {5, 0}) == 6);   // line past the last line clamps to text.size()
}

TEST_CASE("apply_content_changes replaces a single range in place") {
    std::string          text{"pub const x := 5;\n"};
    const nlohmann::json changes{
        {{"range",
          {{"start", {{"line", 0}, {"character", 15}}}, {"end", {{"line", 0}, {"character", 16}}}}},
         {"text", "42"}}};
    CHECK(lsp::apply_content_changes(std::move(text), changes) == "pub const x := 42;\n");
}

TEST_CASE("apply_content_changes applies multiple ranged edits in order") {
    std::string          text{"const a := 1;\nconst b := 2;\n"};
    const nlohmann::json changes{
        {{"range",
          {{"start", {{"line", 0}, {"character", 11}}}, {"end", {{"line", 0}, {"character", 12}}}}},
         {"text", "10"}},
        {{"range",
          {{"start", {{"line", 1}, {"character", 11}}}, {"end", {{"line", 1}, {"character", 12}}}}},
         {"text", "20"}},
    };
    CHECK(lsp::apply_content_changes(std::move(text), changes) ==
          "const a := 10;\nconst b := 20;\n");
}

TEST_CASE("apply_content_changes falls back to a full replace when a change has no range") {
    std::string          text{"stale content"};
    const nlohmann::json changes{{{"text", "brand new content"}}};
    CHECK(lsp::apply_content_changes(std::move(text), changes) == "brand new content");
}

} // namespace ghoti::tests
