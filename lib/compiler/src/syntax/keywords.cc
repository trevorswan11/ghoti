#include "compiler/syntax/keywords.hh"

#include <string_view>
#include <tuple>

#include <stdx/fixed/enum_map.hh>
#include <stdx/fixed/hash_table.hh>
#include <stdx/hash.hh>
#include <stdx/option.hh>

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

} // namespace ghoti::syntax
