#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <stdx/types.hh>

#include "support/diagnostic.hh"

namespace ghoti::lsp {

// How the client counts a position's `character`: UTF-16 code units unless it offered UTF-8
enum class position_encoding : u8 {
    UTF8,
    UTF16,
};

auto               set_position_encoding(position_encoding encoding) noexcept -> void;
[[nodiscard]] auto current_position_encoding() noexcept -> position_encoding;
[[nodiscard]] auto position_encoding_name(position_encoding encoding) noexcept -> std::string_view;

// Byte offset of `pos` within `text`; a line/column past the end clamps to `text.size()`
[[nodiscard]] auto offset_of(std::string_view text, source_location pos) -> usize;

// A byte location in `text` as an LSP `Position`, and back
[[nodiscard]] auto client_position(std::string_view text, source_location loc) -> nlohmann::json;
[[nodiscard]] auto source_position(std::string_view text, const nlohmann::json& position)
    -> source_location;

// Applies a `textDocument/didChange` `contentChanges` array to `text`, in order, materializing
// the resulting full document text. An entry with no "range" key replaces the whole document
[[nodiscard]] auto apply_content_changes(std::string text, const nlohmann::json& changes)
    -> std::string;

} // namespace ghoti::lsp
