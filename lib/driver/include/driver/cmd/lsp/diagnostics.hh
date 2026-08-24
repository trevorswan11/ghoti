#pragma once

#include <nlohmann/json.hpp>

#include "compiler/module/module.hh"

namespace ghoti::lsp {

// Converts a module's diagnostics (if any) into an LSP `Diagnostic[]` JSON array
[[nodiscard]] auto to_lsp_diagnostics(const mod::module& module) -> nlohmann::json;

} // namespace ghoti::lsp
