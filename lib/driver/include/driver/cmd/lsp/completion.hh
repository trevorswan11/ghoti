#pragma once

#include <nlohmann/json.hpp>

#include "compiler/module/module.hh"
#include "support/diagnostic.hh"

namespace ghoti::lsp {

// There's no dedicated Union kind, so unions map to Struct
enum class completion_kind : i32 {
    FUNCTION = 3,
    VARIABLE = 6,
    KEYWORD  = 14,
    CONSTANT = 21,
    STRUCT   = 22,
    ENUM     = 13,
};

// Keyword, top-level-declaration, and local-scope `CompletionItem[]` candidates for `module`
[[nodiscard]] auto completion_items(const mod::module& module, source_location target)
    -> nlohmann::json;

} // namespace ghoti::lsp
