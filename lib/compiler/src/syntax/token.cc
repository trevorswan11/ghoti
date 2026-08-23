#include "compiler/syntax/token.hh"

#include <cctype>
#include <string>

#include <stdx/assert.hh>
#include <stdx/types.hh>

#include "compiler/syntax/token_type.hh"

namespace ghoti::syntax {

auto token_t::materialize_string() const -> std::string {
    ASSERT(type == token_type_t::STRING || type == token_type_t::MULTILINE_STRING);

    // Trim quotes and decode escapes; the lexer only scans past them, never decodes them.
    if (type == token_type_t::STRING) {
        std::string decoded;
        decoded.reserve(slice.size());
        for (auto it{slice.begin() + 1}, end{slice.end() - 1}; it != end; ++it) {
            if (*it != '\\' || it + 1 == end) {
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

auto token_t::is_decl_token() const noexcept -> bool {
    switch (type) {
    case token_type_t::VAR:
    case token_type_t::CONSTANT:
    case token_type_t::CONSTEXPR:
    case token_type_t::PUBLIC:
    case token_type_t::EXTERN:
    case token_type_t::EXPORT:    return true;
    default:                      return false;
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
