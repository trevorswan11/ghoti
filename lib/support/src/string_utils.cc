#include "support/string_utils.hh"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

#include <stdx/types.hh>

namespace ghoti::string_utils {

auto to_u8string(std::string_view s) -> std::u8string {
    std::u8string out;
    out.reserve(s.size());
    for (const auto c : s) { out.push_back(static_cast<char8_t>(c)); }
    return out;
}

auto to_utf8(const std::u8string& s) -> std::string {
    std::string out;
    out.reserve(s.size());
    for (const auto c : s) { out.push_back(static_cast<char>(c)); }
    return out;
}

auto is_alpha(char c) noexcept -> bool { return std::isalpha(static_cast<u8>(c)); }

auto is_digit(char c) noexcept -> bool { return std::isdigit(static_cast<u8>(c)); }

auto is_alnum(char c) noexcept -> bool { return std::isalnum(static_cast<u8>(c)); }

auto is_space(char c) noexcept -> bool { return std::isspace(static_cast<u8>(c)); }

auto is_word(char c) -> bool {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}
auto is_ascii_identifier(std::string_view token) -> bool {
    if (token.empty() || is_digit(token.front())) { return false; }
    return std::ranges::all_of(token, is_word);
}
auto is_identifier_start(char c) -> bool {
    return c == '@' || c == '_' || std::isalpha(static_cast<unsigned char>(c)) != 0;
}

} // namespace ghoti::string_utils
