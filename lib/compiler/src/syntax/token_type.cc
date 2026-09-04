#include "compiler/syntax/token_type.hh"

#include <algorithm>
#include <cctype>
#include <string_view>

#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/keywords.hh"

namespace ghoti::syntax {

auto base_idx(numeric_base base) noexcept -> i32 {
    switch (base) {
    case numeric_base::BINARY:      return 0;
    case numeric_base::OCTAL:       return 1;
    case numeric_base::DECIMAL:     return 2;
    case numeric_base::HEXADECIMAL: return 3;
    }
}

auto digit_in_base(char c, numeric_base base) noexcept -> bool {
    switch (base) {
    case numeric_base::BINARY:      return c == '0' || c == '1';
    case numeric_base::OCTAL:       return c >= '0' && c <= '7';
    case numeric_base::DECIMAL:     return std::isdigit(c);
    case numeric_base::HEXADECIMAL: return std::isxdigit(c);
    default:                        UNREACHABLE("Unknown base");
    }
}

namespace token_type {

auto to_base(token_type_t tt) noexcept -> stdx::option<numeric_base> {
    switch (tt) {
    case token_type_t::INT_2:  return numeric_base::BINARY;
    case token_type_t::INT_8:  return numeric_base::OCTAL;
    case token_type_t::INT_10:
    case token_type_t::REAL:   return numeric_base::DECIMAL;
    case token_type_t::INT_16: return numeric_base::HEXADECIMAL;
    default:                   return stdx::none;
    }
}

auto misc_from_char(char c) noexcept -> stdx::option<token_type_t> {
    switch (c) {
    case ',': return token_type_t::COMMA;
    case ':': return token_type_t::COLON;
    case ';': return token_type_t::SEMICOLON;
    case '(': return token_type_t::LPAREN;
    case ')': return token_type_t::RPAREN;
    case '{': return token_type_t::LBRACE;
    case '}': return token_type_t::RBRACE;
    case '[': return token_type_t::LBRACKET;
    case ']': return token_type_t::RBRACKET;
    default:  return stdx::none;
    }
}

auto is_primitive(token_type_t type) noexcept -> bool {
    return std::ranges::contains(ALL_PRIMITIVES, type);
}

auto is_int_type_lexeme(std::string_view s) noexcept -> bool {
    if (s.size() < 2 || (s[0] != 'i' && s[0] != 'u') || s[1] < '1' || s[1] > '9') { return false; }
    return std::ranges::all_of(s.substr(2), [](char c) { return c >= '0' && c <= '9'; });
}

auto is_valid_ident(token_type_t type) noexcept -> bool {
    switch (type) {
    case token_type_t::IDENT:
    case token_type_t::NORETURN:
    case token_type_t::TYPE_TYPE:
    case token_type_t::AUTO_TYPE:
    case token_type_t::OPAQUE_TYPE: return true;
    default:                        return is_primitive(type) || get_builtin_opt(type);
    }
}

auto is_valid_identifier_name(std::string_view name) noexcept -> bool {
    if (name.empty()) { return false; }
    if (!std::isalpha(static_cast<u8>(name[0])) && name[0] != '_') { return false; }

    for (const auto c : name) {
        if (!std::isalnum(static_cast<u8>(c)) && c != '_') { return false; }
    }
    return true;
}

} // namespace token_type

} // namespace ghoti::syntax
