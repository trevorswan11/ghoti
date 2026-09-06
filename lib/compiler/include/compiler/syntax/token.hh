#pragma once

#include <string>
#include <string_view>

#include <stdx/types.hh>

#include "compiler/syntax/token_type.hh"
#include "support/diagnostic.hh"

namespace ghoti {

namespace syntax {

struct token_t {
    token_type_t     type{};
    std::string_view slice;
    usize            line{};
    usize            column{};

    token_t() noexcept = default;
    token_t(token_type_t tt, std::string_view tok) noexcept : type{tt}, slice{tok} {}
    token_t(token_type_t tt, std::string_view slice, usize line, usize column) noexcept
        : type{tt}, slice{slice}, line{line}, column{column} {}

    explicit token_t(const typed_identifier& tok) noexcept : type{tok.type}, slice{tok.name} {}

    // Materializes the token, asserting that it was a string token
    [[nodiscard]] auto materialize_string() const -> std::string;

    // True when this is an `IDENT` produced from the raw-identifier form `@"..."`.
    [[nodiscard]] auto is_raw_identifier() const noexcept -> bool {
        return type == token_type_t::IDENT && slice.starts_with("@\"");
    }

    // Decodes a raw identifier's `@"..."` lexeme into its bare name, resolving escape sequences.
    // Asserts `is_raw_identifier()`.
    [[nodiscard]] auto materialize_raw_identifier() const -> std::string;

    [[nodiscard]] auto is_decl_token() const noexcept -> bool;

    // Checks if the token can be used to kick off member parsing
    [[nodiscard]] auto is_member_token() const noexcept -> bool;

    // Check whether the token is an ident, primitive type, or builtin function.
    [[nodiscard]] auto is_valid_ident() const noexcept -> bool {
        return token_type::is_valid_ident(type);
    }

    auto operator==(const token_t& other) const noexcept -> bool = default;
};

} // namespace syntax

template <> struct source_info<syntax::token_t> {
    static auto get(const syntax::token_t& t) -> source_location { return {t.line, t.column}; }
};

} // namespace ghoti
