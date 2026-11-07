#pragma once

#include <filesystem>
#include <string_view>

#include <nlohmann/json.hpp>
#include <stdx/function.hh>
#include <stdx/option.hh>

#include "compiler/module/module.hh"
#include "support/diagnostic.hh"

namespace ghoti::lsp {

// The text of another file a label or note points into, for the client's column units
using source_text_lookup =
    stdx::function<stdx::option<std::string_view>(const std::filesystem::path&)>;

// Converts a module's diagnostics (if any) into a sorted LSP `Diagnostic[]` JSON array. The
// range is the primary span, labels and located notes become `relatedInformation`, and the
// unlocated notes are appended to the message
[[nodiscard]] auto to_lsp_diagnostics(const mod::module_t&      module,
                                      const source_text_lookup& lookup = {}) -> nlohmann::json;

// A one-character range at `loc`, the start of the file when there is none
// `text` is the source the location is in, for the client's column units
[[nodiscard]] auto range_of(stdx::option<source_location> loc, std::string_view text)
    -> nlohmann::json;

// A real [start, end) range, for callers that have one
[[nodiscard]] auto range_of(source_span span, std::string_view text) -> nlohmann::json;

} // namespace ghoti::lsp
