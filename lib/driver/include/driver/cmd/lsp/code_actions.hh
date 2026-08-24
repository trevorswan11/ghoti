#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace ghoti::lsp {

// Quick-fix `CodeAction[]` for the diagnostics the client reports in a `textDocument/codeAction`
[[nodiscard]] auto code_actions(const std::string& uri, const nlohmann::json& diagnostics)
    -> nlohmann::json;

} // namespace ghoti::lsp
