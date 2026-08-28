#pragma once

#include <string_view>

#include <nlohmann/json.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

namespace ghoti::lsp {

struct formatting_options {
    u16 indent_spaces{4};
    u16 max_width{100};
};

// Produces a TextEdit[] JSON array for `textDocument/formatting`, or nullptr if formatting fails
[[nodiscard]] auto format(std::string_view source_code, formatting_options opts = {})
    -> nlohmann::json;

} // namespace ghoti::lsp
