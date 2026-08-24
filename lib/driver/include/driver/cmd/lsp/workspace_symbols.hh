#pragma once

#include <string>
#include <vector>

#include "driver/cmd/lsp/document_symbols.hh"
#include "support/diagnostic.hh"

namespace ghoti::lsp {

// One top-level declaration, indexed by document_store across every module it has touched
struct workspace_symbol_entry {
    std::string name;
    symbol_kind kind;
    source_span range;
};

// Every top-level declaration in `module`, for accumulation into a workspace-wide index
[[nodiscard]] auto module_workspace_symbols(const mod::module& module)
    -> std::vector<workspace_symbol_entry>;

} // namespace ghoti::lsp
