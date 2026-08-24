#pragma once

#include <nlohmann/json.hpp>

#include "compiler/module/module.hh"

namespace ghoti::lsp {

// Builds a flat, top-level-only `DocumentSymbol[]` outline of a module
[[nodiscard]] auto document_symbols(const mod::module& module) -> nlohmann::json;

} // namespace ghoti::lsp
