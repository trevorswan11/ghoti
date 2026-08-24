#include "driver/cmd/lsp/position_index.hh"

#include <stdx/option.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/id.hh"
#include "compiler/module/module.hh"
#include "support/diagnostic.hh"

namespace ghoti::lsp {

// A linear scan over resolved identifiers; simple and fast enough for a single file's worth
auto identifier_at(const mod::module& module, source_location target)
    -> stdx::option<ast::node_id> {
    for (const auto id : module.identifier_references) {
        const auto& loc{module.ast.location_of(id)};
        if (loc.line != target.line || target.column < loc.column) { continue; }

        const auto& name{module.ast.get_as<ast::identifier_expr>(id).name};
        if (target.column < loc.column + name.size()) { return id; }
    }
    return stdx::none;
}

} // namespace ghoti::lsp
