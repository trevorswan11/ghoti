#include "driver/cmd/lsp/text_edit.hh"

#include <algorithm>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <stdx/types.hh>

#include "support/diagnostic.hh"

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

auto position_of(const nlohmann::json& pos) -> source_location {
    return {pos.at("line").get<usize>(), pos.at("character").get<usize>()};
}

} // namespace

auto apply_content_changes(std::string text, const nlohmann::json& changes) -> std::string {
    for (const auto& change : changes) {
        if (!change.contains("range")) {
            text = change.at("text").get<std::string>();
            continue;
        }

        const auto& range{change.at("range")};
        const auto  start{offset_of(text, position_of(range.at("start")))};
        const auto  end{offset_of(text, position_of(range.at("end")))};
        text.replace(start, end - start, change.at("text").get<std::string>());
    }
    return text;
}

} // namespace ghoti::lsp
