#pragma once

#include <nlohmann/json.hpp>

#include "compiler/module/module.hh"
#include "support/diagnostic.hh"

namespace ghoti::lsp {

// Keyword, top-level-declaration, and local-scope `CompletionItem[]` candidates for `module`
[[nodiscard]] auto completion_items(const mod::module& module, source_location target)
    -> nlohmann::json;

} // namespace ghoti::lsp
