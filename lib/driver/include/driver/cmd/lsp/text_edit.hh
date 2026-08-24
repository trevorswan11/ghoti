#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <stdx/types.hh>

#include "support/diagnostic.hh"

namespace ghoti::lsp {

// Byte offset of `pos` within `text`; a line/column past the end clamps to `text.size()`
[[nodiscard]] auto offset_of(std::string_view text, source_location pos) -> usize;

// Applies a `textDocument/didChange` `contentChanges` array to `text`, in order, materializing
// the resulting full document text. An entry with no "range" key replaces the whole document
[[nodiscard]] auto apply_content_changes(std::string text, const nlohmann::json& changes)
    -> std::string;

} // namespace ghoti::lsp
