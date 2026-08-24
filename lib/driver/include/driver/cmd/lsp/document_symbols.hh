#pragma once

#include <nlohmann/json.hpp>
#include <stdx/types.hh>

#include "compiler/ast/statement.hh"
#include "compiler/module/module.hh"

namespace ghoti::lsp {

// There's no dedicated Union kind, so unions map to Struct
enum class symbol_kind : i32 {
    ENUM     = 10,
    FUNCTION = 12,
    VARIABLE = 13,
    CONSTANT = 14,
    STRUCT   = 23,
};

// Classifies a top-level decl_stmt for both document and workspace symbol responses
[[nodiscard]] auto symbol_kind_of(const mod::module& module, const ast::decl_stmt& decl)
    -> symbol_kind;

// Builds a flat, top-level-only `DocumentSymbol[]` outline of a module
[[nodiscard]] auto document_symbols(const mod::module& module) -> nlohmann::json;

} // namespace ghoti::lsp
