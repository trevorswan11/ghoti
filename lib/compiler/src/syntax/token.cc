#include "compiler/syntax/token.hh"

#include <array>
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
        // The lexer rejects unknown escapes, so the fallback only guards direct construction
        decoded.push_back(decode_escape(*it).value_or(*it));
    }
    return decoded;
}

} // namespace

auto is_valid_utf8(std::string_view bytes) noexcept -> bool {
    // Thanks claude
    usize i{0};
    while (i < bytes.size()) {
        const auto lead{static_cast<u8>(bytes[i])};
        usize      extra{0};
        u32        code_point{0};
        if (lead < 0x80) {
            ++i;
            continue;
        }
        if ((lead & 0xE0U) == 0xC0U) {
            extra      = 1;
            code_point = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0U) {
            extra      = 2;
            code_point = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0U) {
            extra      = 3;
            code_point = lead & 0x07U;
        } else {
            return false;
        }
        if (i + extra >= bytes.size()) { return false; }
        for (usize k{1}; k <= extra; ++k) {
            const auto cont{static_cast<u8>(bytes[i + k])};
            if ((cont & 0xC0U) != 0x80U) { return false; }
            code_point = (code_point << 6U) | (cont & 0x3FU);
        }
        constexpr std::array<u32, 4> min_for_length{0, 0x80, 0x800, 0x10000};
        const bool                   overlong{code_point < min_for_length[extra]};
        const bool                   surrogate{code_point >= 0xD800 && code_point <= 0xDFFF};
        if (overlong || surrogate || code_point > 0x10FFFF) { return false; }
        i += extra + 1;
    }
    return true;
}

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

        // Skip indentation followed by a double backslash at start of line to clean the string
        if (at_line_start) {
            usize marker{i};
            while (marker < slice.size() && (slice[marker] == ' ' || slice[marker] == '\t')) {
                marker += 1;
            }
            if (marker + 1 < slice.size() && slice[marker] == '\\' && slice[marker + 1] == '\\') {
                i = marker + 1;
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
    case token_type_t::AT_LBRACKET: return true;
    default:                        return false;
    }
}

auto token_t::is_member_token() const noexcept -> bool {
    switch (type) {
    case token_type_t::IMPORT: return true;
    default:                   return is_decl_token();
    }
}

} // namespace ghoti::syntax
