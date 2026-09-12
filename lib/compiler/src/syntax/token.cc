#include "compiler/syntax/token.hh"

#include <iterator>
#include <string>
#include <string_view>

#include <stdx/assert.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>

#include "compiler/syntax/token_type.hh"

namespace ghoti::syntax {

namespace {

// Decodes the C-style escape sequences ghoti recognizes inside a delimited literal
auto decode_escapes(std::string_view inner) -> std::string {
    std::string decoded;
    decoded.reserve(inner.size());
    for (auto it{inner.begin()}, end{inner.end()}; it != end; ++it) {
        if (*it != '\\' || std::next(it) == end) {
            decoded.push_back(*it);
            continue;
        }
        ++it;
        switch (*it) {
        case 'n':  decoded.push_back('\n'); break;
        case 'r':  decoded.push_back('\r'); break;
        case 't':  decoded.push_back('\t'); break;
        case '\\': decoded.push_back('\\'); break;
        case '\'': decoded.push_back('\''); break;
        case '"':  decoded.push_back('"'); break;
        case '0':  decoded.push_back('\0'); break;
        default:   decoded.push_back(*it); break;
        }
    }
    return decoded;
}

} // namespace

auto token_t::materialize_string() const -> std::string {
    ASSERT(type == token_type_t::STRING || type == token_type_t::MULTILINE_STRING);

    // Trim quotes and decode escapes; the lexer only scans past them, never decodes them.
    if (type == token_type_t::STRING) {
        return decode_escapes(stdx::string::substr(slice, 1, slice.size() - 2));
    }

    std::string builder{};
    builder.reserve(slice.size());

    auto at_line_start{true};
    for (usize i{0}; i < slice.size(); ++i) {
        const auto c{slice[i]};

        // Skip a double backslash at start of line to clean the string
        if (at_line_start) {
            if (c == '\\' && i + 1 < slice.size() && slice[i + 1] == '\\') {
                i += 1;
                continue;
            }
            at_line_start = false;
        }

        builder.push_back(c);
        if (c == '\n') { at_line_start = true; }
    }

    return builder;
}

auto token_t::materialize_raw_identifier() const -> std::string {
    ASSERT(is_raw_identifier());
    // Strip the leading `@"` and the trailing `"`, then decode escapes.
    return decode_escapes(stdx::string::substr(slice, 2, slice.size() - 3));
}

auto token_t::is_decl_token() const noexcept -> bool {
    switch (type) {
    case token_type_t::VAR:
    case token_type_t::CONSTANT:
    case token_type_t::CONSTEXPR:
    case token_type_t::PUBLIC:
    case token_type_t::EXTERN:
    case token_type_t::EXPORT:
    case token_type_t::THREADLOCAL:
    case token_type_t::WEAK:
    case token_type_t::BUILTIN_DISCARDABLE: return true;
    default:                                return false;
    }
}

auto token_t::is_member_token() const noexcept -> bool {
    switch (type) {
    case token_type_t::IMPORT:
    case token_type_t::USING:  return true;
    default:                   return is_decl_token();
    }
}

} // namespace ghoti::syntax
