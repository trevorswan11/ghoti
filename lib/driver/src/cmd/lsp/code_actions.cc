#include "driver/cmd/lsp/code_actions.hh"

#include <array>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include <stdx/string.hh>

namespace ghoti::lsp {

namespace {

constexpr std::array<std::pair<std::string_view, std::string_view>, 6> INSERTABLE_TOKENS{{
    {"SEMICOLON", ";"},
    {"RBRACE", "}"},
    {"RPAREN", ")"},
    {"RBRACKET", "]"},
    {"COLON", ":"},
    {"COMMA", ","},
}};

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
        {
            "edit",
            {
                {"changes", std::move(changes)},
            },
        },
    };
}

} // namespace

auto code_actions(const std::string& uri, const nlohmann::json& diagnostics) -> nlohmann::json {
    constexpr std::string_view expected_token_prefix{"Expected token "};

    auto out = nlohmann::json::array();

    for (const auto& diag : diagnostics) {
        const auto  code{diag.value("code", std::string{})};
        const auto  message{diag.value("message", std::string{})};
        const auto& start{diag.at("range").at("start")};

        if (code == "UNEXPECTED_TOKEN" && message.starts_with(expected_token_prefix)) {
            // Message shape: "Expected token <NAME>, found <NAME>"
            const std::string_view after_prefix{message.data() + expected_token_prefix.size(),
                                                message.size() - expected_token_prefix.size()};
            const auto expected_name{stdx::string::substr(after_prefix, 0, after_prefix.find(','))};
            for (const auto& [name, spelling] : INSERTABLE_TOKENS) {
                if (expected_name != name) { continue; }
                out.push_back(quick_fix(
                    "Insert missing '" + std::string{spelling} + "'", uri, start, spelling, diag));
                break;
            }
        } else if (code == "ILLEGAL_DECL_MODIFIERS" &&
                   message == "Exactly one mutability modifier may be used; found 0") {
            out.push_back(quick_fix("Add 'const' modifier", uri, start, "const ", diag));
            out.push_back(quick_fix("Add 'var' modifier", uri, start, "var ", diag));
        }
    }

    return out;
}

} // namespace ghoti::lsp
