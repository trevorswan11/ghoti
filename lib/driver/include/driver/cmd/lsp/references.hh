#pragma once

#include <vector>

#include <stdx/option.hh>

#include "compiler/module/module.hh"
#include "support/diagnostic.hh"

namespace ghoti::lsp {

// Resolves the identifier at `target` to its canonical declaration location
[[nodiscard]] auto definition_location_at(const mod::module& module, source_location target)
    -> stdx::option<located_span>;

// Every reference to `definition` across every module in `manager`, not including the
// declaration itself. Only finds references within `manager`'s loaded module graph
[[nodiscard]] auto find_references(const mod::module_manager& manager,
                                   const located_span& definition) -> std::vector<located_span>;

} // namespace ghoti::lsp
