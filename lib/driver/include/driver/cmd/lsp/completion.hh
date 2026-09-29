#pragma once

#include <string_view>

#include <nlohmann/json.hpp>
#include <stdx/option.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/module/module.hh"
#include "support/diagnostic.hh"

namespace ghoti::lsp {

// There's no dedicated Union kind, so unions map to Struct
enum class completion_kind : i32 {
    FUNCTION = 3,
    VARIABLE = 6,
    KEYWORD  = 14,
    CONSTANT = 21,
    STRUCT   = 22,
    ENUM     = 13,
    PROPERTY = 10,
    MEMBER   = 20,
};

// Where a position sits inside an unclosed `@[...]` attribute list
struct attribute_context {
    // Set inside the parentheses of this attribute's arguments
    stdx::option<ast::attribute_kind> in_args_of{};
    // The identifier being typed (or hovered), possibly empty
    std::string_view word{};
    // The word directly follows a `.`, as in `visibility(.hid`
    bool after_dot{false};
};

[[nodiscard]] auto attribute_context_at(std::string_view source, source_location target)
    -> stdx::option<attribute_context>;

// Keyword, top-level-declaration, and local-scope `CompletionItem[]` candidates for `module`, or
// attribute names and arguments inside an `@[...]` list
[[nodiscard]] auto completion_items(const mod::module& module, source_location target)
    -> nlohmann::json;

} // namespace ghoti::lsp
