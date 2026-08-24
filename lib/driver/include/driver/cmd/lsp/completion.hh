#pragma once

#include <nlohmann/json.hpp>

#include "compiler/module/module.hh"

namespace ghoti::lsp {

// Keyword and top-level-declaration `CompletionItem[]` candidates for a module.
//
// TODO: extend to local-scope and member-access completion yet
[[nodiscard]] auto completion_items(const mod::module& module) -> nlohmann::json;

} // namespace ghoti::lsp
