#pragma once

#include <algorithm>
#include <string_view>
#include <vector>

#include <gsl/span>
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
    ALIGN,
    DEPRECATED,
    VISIBILITY,
};

// Mirrors `builtin.BranchHint`
enum class branch_hint : u8 {
    NONE,
    LIKELY,
    UNLIKELY,
    COLD,
    UNPREDICTABLE,
};

[[nodiscard]] auto branch_hint_from_name(std::string_view name) noexcept
    -> stdx::option<branch_hint>;

// Mirrors `builtin.Inline`
enum class inline_mode : u8 {
    ALWAYS,
    NEVER,
    HINT,
    DEFAULT,
};

[[nodiscard]] auto inline_mode_name(inline_mode mode) noexcept -> std::string_view;

[[nodiscard]] auto inline_mode_from_name(std::string_view name) noexcept
    -> stdx::option<inline_mode>;

// Mirrors `builtin.Visibility`: how far outside its own linked image a symbol is seen
enum class symbol_visibility : u8 {
    DEFAULT,
    HIDDEN,
    PROTECTED,
};

[[nodiscard]] auto symbol_visibility_name(symbol_visibility visibility) noexcept
    -> std::string_view;

[[nodiscard]] auto symbol_visibility_from_name(std::string_view name) noexcept
    -> stdx::option<symbol_visibility>;

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
    std::string_view signature; // For tooling, e.g. `visibility(builtin.Visibility)`
    std::string_view doc;
};

[[nodiscard]] auto attribute_spec_of(std::string_view name) noexcept
    -> stdx::option<const attribute_spec&>;

// Assumes the kind is a valid enumeration
[[nodiscard]] auto attribute_spec_of(attribute_kind kind) noexcept -> const attribute_spec&;
[[nodiscard]] auto all_attribute_specs() noexcept -> gsl::span<const attribute_spec>;

// The variant names an enum-valued attribute accepts, empty for any other attribute
[[nodiscard]] auto attribute_enum_variants(attribute_kind kind) noexcept
    -> gsl::span<const std::string_view>;

// Applies to a function definition, so on a declaration initialized by a function literal it
// reaches through to that literal (and sees the literal's parameters)
[[nodiscard]] auto routes_to_fn_literal(attribute_kind kind) noexcept -> bool;

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
        if (const auto it{std::ranges::find(items, kind, &attribute::kind)}; it != items.end()) {
            return *it;
        }
        return stdx::none;
    }
};

// The first argument of `kind` in `attributes`, if that attribute is present
[[nodiscard]] auto attribute_arg(const stdx::option<attribute_list>& attributes,
                                 attribute_kind kind) noexcept -> stdx::option<expr_handle>;

// Parses `@[...]` with the current token on `@[`, leaving it on the closing `]`
[[nodiscard]] auto parse_attribute_list(syntax::parser& parser)
    -> stdx::result<attribute_list, syntax::diagnostic>;

} // namespace ghoti::ast
