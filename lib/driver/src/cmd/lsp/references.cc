#include "driver/cmd/lsp/references.hh"

#include <vector>

#include <stdx/option.hh>

#include "compiler/module/module.hh"
#include "driver/cmd/lsp/position_index.hh"
#include "support/diagnostic.hh"

namespace ghoti::lsp {

auto definition_location_at(const mod::module& module, source_location target)
    -> stdx::option<located_span> {
    const auto id{identifier_at(module, target)};
    if (!id) { return stdx::none; }
    if (const auto def{module.get_identifier_definition(*id)}) { return def; }

    // Not a reference, so `id` must be a declaration name; it's its own canonical span
    return located_span{module.path,
                        {module.ast.location_of(*id), module.ast.end_location_of(*id)}};
}

auto find_references(const mod::module_manager& manager, const located_span& definition)
    -> std::vector<located_span> {
    std::vector<located_span> results;
    for (const auto& [path, mod] : manager) {
        for (const auto id : mod->identifier_positions) {
            const auto def{mod->get_identifier_definition(id)};
            if (def && *def == definition) {
                results.emplace_back<located_span>({
                    .path = path,
                    .span = {mod->ast.location_of(id), mod->ast.end_location_of(id)},
                });
            }
        }
    }
    return results;
}

} // namespace ghoti::lsp
