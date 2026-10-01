#include "driver/cmd/lsp/code_actions.hh"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include <stdx/option.hh>
#include <stdx/string.hh>

namespace ghoti::lsp {

namespace {

// Tokens a missing-token diagnostic can be fixed by inserting verbatim
constexpr std::array<std::string_view, 6> INSERTABLE_TOKENS{";", "}", ")", "]", ":", ","};

// A missing-token diagnostic reads "Expected '<spelling>', found ..."
[[nodiscard]] auto expected_spelling(std::string_view message) -> stdx::option<std::string_view> {
    constexpr std::string_view prefix{"Expected '"};
    if (!message.starts_with(prefix)) { return stdx::none; }
    const auto rest{stdx::string::substr(message, prefix.size())};
    const auto close{rest.find("',")};
    if (close == std::string_view::npos) { return stdx::none; }
    return stdx::string::substr(rest, 0, close);
}

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
    auto out = nlohmann::json::array();

    for (const auto& diag : diagnostics) {
        const auto  code{diag.value("code", std::string{})};
        const auto  message{diag.value("message", std::string{})};
        const auto& start{diag.at("range").at("start")};

        const auto spelling{code == "UNEXPECTED_TOKEN" ? expected_spelling(message) : stdx::none};
        if (spelling && std::ranges::contains(INSERTABLE_TOKENS, *spelling)) {
            out.push_back(quick_fix(
                "Insert missing '" + std::string{*spelling} + "'", uri, start, *spelling, diag));
        } else if (code == "ILLEGAL_DECL_MODIFIERS" &&
                   message == "Exactly one of 'const', 'let' or 'let mut' may be used; found 0") {
            out.push_back(quick_fix("Add 'const'", uri, start, "const ", diag));
            out.push_back(quick_fix("Add 'let'", uri, start, "let ", diag));
            out.push_back(quick_fix("Add 'let mut'", uri, start, "let mut ", diag));
        }
    }

    return out;
}

} // namespace ghoti::lsp
