#pragma once

#include <string_view>
#include <vector>

#include <stdx/enum.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/handle.hh"
#include "compiler/syntax/error.hh"

namespace ghoti::syntax { class parser; } // namespace ghoti::syntax

namespace ghoti::ast {

// C is the default and  means "whatever the target ABI dictates"
enum class calling_convention : u8 {
    C,
    SYSV,
    WIN64,
    X86_STDCALL,
    X86_FASTCALL,
    AAPCS,
};

[[nodiscard]] auto calling_convention_from_name(std::string_view name) noexcept
    -> stdx::option<calling_convention>;
[[nodiscard]] auto calling_convention_name(calling_convention conv) noexcept -> std::string_view;

enum class attribute_kind : u8 {
    DISCARDABLE,
    INLINE,
    NAKED,
};

// Mirrors `builtin.Inline`
enum class inline_mode : u8 {
    ALWAYS,
    NEVER,
    HINT,
};

[[nodiscard]] auto inline_mode_from_name(std::string_view name) noexcept
    -> stdx::option<inline_mode>;

// What an attribute may annotate. `FN_DECL` is a declaration whose type is callable.
enum class attribute_target : u8 {
    DECL    = 1 << 0,
    FN_DECL = 1 << 1,
    FN      = 1 << 2,
    FIELD   = 1 << 3,
};

MAKE_ENUM_OPERATORS(attribute_target)

struct attribute_spec {
    std::string_view name;
    attribute_kind   kind;
    u8               min_args;
    u8               max_args;
    attribute_target targets;
};

[[nodiscard]] auto attribute_spec_of(std::string_view name) noexcept
    -> stdx::option<const attribute_spec&>;
[[nodiscard]] auto attribute_spec_of(attribute_kind kind) noexcept -> const attribute_spec&;

// Applies only to a function definition, so on a declaration it reaches through to the initializer
[[nodiscard]] auto is_function_only(attribute_kind kind) noexcept -> bool;

// One `name` or `name(args...)` entry of an `@[...]` list
struct attribute {
    identifier_handle        name;
    attribute_kind           kind;
    std::vector<expr_handle> args;
};

struct attribute_list {
    std::vector<attribute> items;
    bool                   force_break{false}; // `@[a, b,]`: keep the list on its own line

    [[nodiscard]] auto find(attribute_kind kind) const noexcept -> stdx::option<const attribute&> {
        for (const auto& item : items) {
            if (item.kind == kind) { return item; }
        }
        return stdx::none;
    }
};

// Parses `@[...]` with the current token on `@[`, leaving it on the closing `]`
[[nodiscard]] auto parse_attribute_list(syntax::parser& parser)
    -> stdx::result<attribute_list, syntax::diagnostic>;

} // namespace ghoti::ast
