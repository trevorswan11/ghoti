#include "driver/cmd/lsp/references.hh"

#include <vector>

#include <stdx/option.hh>

#include "compiler/module/module.hh"
#include "driver/cmd/lsp/position_index.hh"
#include "support/diagnostic.hh"

namespace ghoti::lsp {

auto definition_span_at(const mod::module& module, source_location target)
    -> stdx::option<source_span> {
    const auto id{identifier_at(module, target)};
    if (!id) { return stdx::none; }
    if (const auto def{module.get_identifier_definition(*id)}) { return def; }

    // Not a reference, so `id` must be a declaration name; it's its own canonical span
    return source_span{module.ast.location_of(*id), module.ast.end_location_of(*id)};
}

auto find_references(const mod::module& module, source_span definition)
    -> std::vector<source_span> {
    std::vector<source_span> results;
    for (const auto id : module.identifier_positions) {
        const auto def{module.get_identifier_definition(id)};
        if (def && *def == definition) {
            results.emplace_back(module.ast.location_of(id), module.ast.end_location_of(id));
        }
    }
    return results;
}

} // namespace ghoti::lsp
