#include "compiler/syntax/keywords.hh"

#include <algorithm>
#include <string>
#include <string_view>
#include <tuple>

#include <fmt/format.h>
#include <stdx/fixed/enum_map.hh>
#include <stdx/fixed/hash_table.hh>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/syntax/token.hh"
#include "compiler/syntax/token_type.hh"
#include "support/string_utils.hh"

namespace ghoti::syntax {

namespace {

constexpr auto KEYWORD_LOOKUP{
    std::apply([](auto&&... kw) { return string_utils::make_constexpr_map<token_type_t>(kw...); },
               ALL_KEYWORDS)};

constexpr auto ALL_KEYWORDS_TT{[] -> auto {
    stdx::fixed::enum_map<token_type_t, stdx::option<std::string_view>> keywords;
    for (const auto& [name, tok] : KEYWORD_LOOKUP) { keywords[tok] = name; }
    return keywords;
}()};

} // namespace

auto get_keyword_opt(std::string_view sv) noexcept -> stdx::option<token_type_t> {
    return KEYWORD_LOOKUP.get_opt(sv).materialize();
}

auto get_keyword_opt(token_type_t tt) noexcept -> stdx::option<std::string_view> {
    return ALL_KEYWORDS_TT[tt];
}

auto ascii_spelling(std::string_view name) -> std::string {
    const bool ascii{std::ranges::all_of(name, [](char c) { return static_cast<u8>(c) < 0x80; })};
    if (ascii) { return std::string{name}; }
    std::string spelled{"@\""};
    for (usize i{0}; i < name.size();) {
        const auto decoded{decode_code_point(name.substr(i))};
        if (!decoded) {
            spelled += fmt::format("\\x{:02X}", static_cast<u8>(name[i]));
            ++i;
            continue;
        }
        if (decoded->value < 0x80) {
            spelled.push_back(name[i]);
        } else {
            spelled += fmt::format("\\u{{{:X}}}", decoded->value);
        }
        i += decoded->length;
    }
    spelled += '"';
    return spelled;
}

auto identifier_needs_raw(std::string_view name) noexcept -> bool {
    // Implicitly covers builtin-style names
    if (!token_type::is_valid_identifier_name(name)) { return true; }
    if (name == "_") { return true; }
    if (get_keyword_opt(name)) { return true; }
    return token_type::is_int_type_lexeme(name);
}

} // namespace ghoti::syntax
