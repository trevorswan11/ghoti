#pragma once

#include <vector>

#include <stdx/option.hh>

#include "compiler/module/module.hh"
#include "support/diagnostic.hh"

namespace ghoti::lsp {

// Resolves the identifier at `target` to its canonical declaration span
[[nodiscard]] auto definition_span_at(const mod::module& module, source_location target)
    -> stdx::option<source_span>;

// Every reference resolving to `definition`, not including the declaration itself
[[nodiscard]] auto find_references(const mod::module& module, source_span definition)
    -> std::vector<source_span>;

} // namespace ghoti::lsp
