#pragma once

#include <nlohmann/json.hpp>
#include <stdx/option.hh>

#include "compiler/module/module.hh"
#include "support/diagnostic.hh"

namespace ghoti::lsp {

// Converts a module's diagnostics (if any) into an LSP `Diagnostic[]` JSON array
[[nodiscard]] auto to_lsp_diagnostics(const mod::module& module) -> nlohmann::json;

// Diagnostics only carry a start point today, so end is a same-line one-character placeholder
// TODO: change diags to carry more data
[[nodiscard]] auto range_of(stdx::option<source_location> loc) -> nlohmann::json;

// A real [start, end) range, for callers that have one
[[nodiscard]] auto range_of(source_span span) -> nlohmann::json;

} // namespace ghoti::lsp
