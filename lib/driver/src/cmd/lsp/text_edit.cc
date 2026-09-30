#include "driver/cmd/lsp/text_edit.hh"

#include <algorithm>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <stdx/types.hh>

#include "support/diagnostic.hh"
#include "support/unicode.hh"

namespace ghoti::lsp {

auto offset_of(std::string_view text, source_location pos) -> usize {
    usize offset{0};
    for (usize line{0}; line < pos.line; ++line) {
        const auto newline{text.find('\n', offset)};
        if (newline == std::string_view::npos) { return text.size(); }
        offset = newline + 1;
    }
    const auto line_end{text.find('\n', offset)};
    const auto line_len{(line_end == std::string_view::npos ? text.size() : line_end) - offset};
    return offset + std::min(pos.column, line_len);
}

namespace {

// Compiler locations count bytes, like UTF-8; clients count UTF-16 units unless told otherwise
position_encoding negotiated_encoding{position_encoding::UTF16};

[[nodiscard]] auto line_text(std::string_view text, usize line) -> std::string_view {
    const auto start{offset_of(text, {line, 0})};
    const auto end{text.find('\n', start)};
    auto       result{
        text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start)};
    if (result.ends_with('\r')) { result.remove_suffix(1); }
    return result;
}

} // namespace

auto set_position_encoding(position_encoding encoding) noexcept -> void {
    negotiated_encoding = encoding;
}

auto current_position_encoding() noexcept -> position_encoding { return negotiated_encoding; }

auto position_encoding_name(position_encoding encoding) noexcept -> std::string_view {
    return encoding == position_encoding::UTF8 ? "utf-8" : "utf-16";
}

auto client_position(std::string_view text, source_location loc) -> nlohmann::json {
    auto character{loc.column};
    if (negotiated_encoding == position_encoding::UTF16) {
        const auto line{line_text(text, loc.line)};
        character = utf16_length(line.substr(0, std::min(loc.column, line.size()))) +
                    (loc.column > line.size() ? loc.column - line.size() : 0);
    }
    return {{"line", loc.line}, {"character", character}};
}

auto source_position(std::string_view text, const nlohmann::json& position) -> source_location {
    const auto line{position.at("line").get<usize>()};
    const auto character{position.at("character").get<usize>()};
    if (negotiated_encoding == position_encoding::UTF8) { return {line, character}; }
    return {line, byte_offset_of_utf16(line_text(text, line), character)};
}

auto apply_content_changes(std::string text, const nlohmann::json& changes) -> std::string {
    for (const auto& change : changes) {
        if (!change.contains("range")) {
            text = change.at("text").get<std::string>();
            continue;
        }

        const auto& range{change.at("range")};
        const auto  start{offset_of(text, source_position(text, range.at("start")))};
        const auto  end{offset_of(text, source_position(text, range.at("end")))};
        text.replace(start, end - start, change.at("text").get<std::string>());
    }
    return text;
}

} // namespace ghoti::lsp
