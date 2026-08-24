#include "driver/cmd/lsp/code_actions.hh"

#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace ghoti::lsp {

namespace {

auto quick_fix(std::string_view      title,
               const std::string&    uri,
               const nlohmann::json& insert_at,
               std::string_view      insert_text,
               const nlohmann::json& diagnostic) -> nlohmann::json {
    auto edits = nlohmann::json::array();
    edits.push_back({{"range", {{"start", insert_at}, {"end", insert_at}}},
                     {"newText", std::string{insert_text}}});

    auto changes     = nlohmann::json::object();
    changes[uri]     = std::move(edits);
    auto diagnostics = nlohmann::json::array();
    diagnostics.push_back(diagnostic);

    return {
        {"title", std::string{title}},
        {"kind", "quickfix"},
        {"diagnostics", std::move(diagnostics)},
        {"edit", {{"changes", std::move(changes)}}},
    };
}

} // namespace

auto code_actions(const std::string& uri, const nlohmann::json& diagnostics) -> nlohmann::json {
    auto out = nlohmann::json::array();

    for (const auto& diag : diagnostics) {
        const auto  code{diag.value("code", std::string{})};
        const auto  message{diag.value("message", std::string{})};
        const auto& start{diag.at("range").at("start")};

        // TODO: Expand this LUT to capture more safe actions
        if (code == "UNEXPECTED_TOKEN" && message.starts_with("Expected token SEMICOLON, found ")) {
            out.push_back(quick_fix("Insert missing ';'", uri, start, ";", diag));
        } else if (code == "ILLEGAL_DECL_MODIFIERS" &&
                   message == "Exactly one mutability modifier may be used; found 0") {
            out.push_back(quick_fix("Add 'const' modifier", uri, start, "const ", diag));
        }
    }

    return out;
}

} // namespace ghoti::lsp
