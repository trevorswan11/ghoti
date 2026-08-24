#include "driver/cmd/lsp/workspace_symbols.hh"

#include <string>
#include <vector>

#include "compiler/ast/expression.hh"
#include "compiler/ast/statement.hh"
#include "compiler/module/module.hh"
#include "driver/cmd/lsp/document_symbols.hh"

namespace ghoti::lsp {

auto module_workspace_symbols(const mod::module& module) -> std::vector<workspace_symbol_entry> {
    std::vector<workspace_symbol_entry> out;

    for (const auto root_id : module.ast) {
        const auto decl{module.ast.get_as_opt<ast::decl_stmt>(root_id)};
        if (!decl) { continue; }
        const auto& name_ident{module.ast.get_as<ast::identifier_expr>(decl->name)};

        out.push_back({
            .name  = std::string{name_ident.name},
            .kind  = symbol_kind_of(module, *decl),
            .range = {module.ast.location_of(decl->name), module.ast.end_location_of(decl->name)},
        });
    }

    return out;
}

} // namespace ghoti::lsp
