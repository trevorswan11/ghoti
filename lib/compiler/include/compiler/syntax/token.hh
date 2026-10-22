#pragma once

#include <string>
#include <string_view>

#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/syntax/token_type.hh"
#include "support/diagnostic.hh"

namespace ghoti {

namespace syntax {

// The byte a `\c` escape sequence denotes, or `none` when ghoti does not recognize `c`.
[[nodiscard]] constexpr auto decode_escape(char code) noexcept -> stdx::option<char> {
    switch (code) {
    case 'n':  return '\n';
    case 'r':  return '\r';
    case 't':  return '\t';
    case '\\': return '\\';
    case '\'': return '\'';
    case '"':  return '"';
    case '0':  return '\0';
    default:   return stdx::none;
    }
}

// Whether `bytes` is well-formed UTF-8 (no overlongs, surrogates, or code points past U+10FFFF)
[[nodiscard]] auto is_valid_utf8(std::string_view bytes) noexcept -> bool;

// One escape sequence read from the start of `text`, which begins with its backslash
struct escape_scan {
    usize       length{0}; // bytes consumed, backslash included
    std::string bytes;     // what the escape stands for: one byte, or a scalar value as UTF-8
    u32         value{0};  // the byte's value, or the scalar value
    std::string error;     // empty when the escape is well-formed
};

// `\n`-style escapes, `\xHH` for one byte, and `\u{H...}` for a Unicode scalar value
[[nodiscard]] auto scan_escape(std::string_view text) -> escape_scan;

[[nodiscard]] auto encode_utf8(u32 code_point) -> std::string;

struct decoded_code_point {
    u32   value;
    usize length;
};

// The UTF-8 code point at the start of `text`; none when it's malformed
[[nodiscard]] auto decode_code_point(std::string_view text) noexcept
    -> stdx::option<decoded_code_point>;

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
