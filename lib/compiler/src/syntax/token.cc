#include "compiler/syntax/token.hh"

#include <array>
#include <iterator>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>

#include "compiler/syntax/token_type.hh"

namespace ghoti::syntax {

namespace {

// Decodes the escape sequences ghoti recognizes inside a delimited literal
auto decode_escapes(std::string_view inner) -> std::string {
    std::string decoded;
    decoded.reserve(inner.size());
    for (usize i{0}; i < inner.size();) {
        if (inner[i] != '\\') {
            decoded.push_back(inner[i++]);
            continue;
        }
        // The lexer rejects malformed escapes, so the fallback only guards direct construction
        const auto scan{scan_escape(inner.substr(i))};
        decoded += scan.error.empty() ? scan.bytes : inner.substr(i, scan.length);
        i += scan.length;
    }
    return decoded;
}

[[nodiscard]] constexpr auto hex_value(char c) noexcept -> stdx::option<u32> {
    if (c >= '0' && c <= '9') { return static_cast<u32>(c - '0'); }
    if (c >= 'a' && c <= 'f') { return static_cast<u32>(c - 'a' + 10); }
    if (c >= 'A' && c <= 'F') { return static_cast<u32>(c - 'A' + 10); }
    return stdx::none;
}

} // namespace

auto scan_escape(std::string_view text) -> escape_scan {
    ASSERT(!text.empty() && text.front() == '\\');
    const auto failed{[](usize length, std::string error) {
        return escape_scan{.length = length, .bytes = {}, .value = 0, .error = std::move(error)};
    }};
    const auto decoded{[](usize length, std::string bytes, u32 value) {
        return escape_scan{
            .length = length, .bytes = std::move(bytes), .value = value, .error = {}};
    }};
    if (text.size() < 2) { return failed(1, "Incomplete escape sequence"); }
    const auto code{text[1]};
    if (const auto simple{decode_escape(code)}) {
        return decoded(2, std::string(1, *simple), static_cast<u8>(*simple));
    }

    if (code == 'x') {
        const auto high{text.size() > 2 ? hex_value(text[2]) : stdx::none};
        const auto low{text.size() > 3 ? hex_value(text[3]) : stdx::none};
        if (!high || !low) { return failed(high ? 3 : 2, "Expected two hex digits after '\\x'"); }
        const auto byte{(*high << 4U) | *low};
        return decoded(4, std::string(1, static_cast<char>(byte)), byte);
    }

    if (code == 'u') {
        if (text.size() < 3 || text[2] != '{') { return failed(2, "Expected '{' after '\\u'"); }
        usize i{3};
        u32   value{0};
        while (i < text.size() && hex_value(text[i])) {
            if (i - 3 < 6) { value = (value << 4U) | *hex_value(text[i]); }
            ++i;
        }
        const auto digits{i - 3};
        if (digits == 0) { return failed(i, "Expected hex digits in '\\u{...}'"); }
        if (i >= text.size() || text[i] != '}') {
            return failed(i, "Expected '}' to close '\\u{...}'");
        }
        ++i;
        if (digits > 6) { return failed(i, "'\\u{...}' takes 1 to 6 hex digits"); }
        if (value > 0x10FFFF) {
            return failed(
                i, fmt::format("U+{:X} is past the last Unicode code point, U+10FFFF", value));
        }
        if (value >= 0xD800 && value <= 0xDFFF) {
            return failed(i,
                          fmt::format("U+{:X} is a surrogate, not a Unicode scalar value", value));
        }
        return decoded(i, encode_utf8(value), value);
    }

    return failed(2, fmt::format("Unknown escape sequence '\\{}'", code));
}

auto encode_utf8(u32 code_point) -> std::string {
    std::string bytes;
    if (code_point < 0x80) {
        bytes.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        bytes.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        bytes.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point < 0x10000) {
        bytes.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        bytes.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        bytes.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        bytes.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
        bytes.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
        bytes.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        bytes.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
    return bytes;
}

auto decode_code_point(std::string_view text) noexcept -> stdx::option<decoded_code_point> {
    if (text.empty()) { return stdx::none; }
    const auto lead{static_cast<u8>(text.front())};
    usize      length{1};
    if (lead >= 0xF0) {
        length = 4;
    } else if (lead >= 0xE0) {
        length = 3;
    } else if (lead >= 0xC0) {
        length = 2;
    }
    if (length > text.size() || !is_valid_utf8(text.substr(0, length))) { return stdx::none; }
    u32 value{length == 1 ? lead : lead & (0xFFU >> (length + 1))};
    for (usize k{1}; k < length; ++k) {
        value = (value << 6U) | (static_cast<u8>(text[k]) & 0x3FU);
    }
    return decoded_code_point{value, length};
}

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
