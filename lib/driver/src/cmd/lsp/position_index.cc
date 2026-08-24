#include "driver/cmd/lsp/position_index.hh"

#include <stdx/option.hh>

#include "compiler/ast/id.hh"
#include "compiler/module/module.hh"
#include "support/diagnostic.hh"

namespace ghoti::lsp {

// A linear scan over resolved identifiers; simple and fast enough for a single file's worth
auto identifier_at(const mod::module& module, source_location target)
    -> stdx::option<ast::node_id> {
    for (const auto id : module.identifier_references) {
        const auto& start{module.ast.location_of(id)};
        const auto& end{module.ast.end_location_of(id)};
        if (start.line != target.line || target.column < start.column) { continue; }
        if (target.column < end.column) { return id; }
    }
    return stdx::none;
}

} // namespace ghoti::lsp
