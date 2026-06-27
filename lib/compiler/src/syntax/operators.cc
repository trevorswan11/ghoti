#include "syntax/operators.hh"

#include <algorithm>
#include <array>
#include <string_view>

#include <stdx/fixed/hash_table.hh>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "syntax/token_type.hh"

namespace ghoti::syntax {

namespace {

constexpr auto ALL_OPERATORS{[] {
    constexpr std::array operators{operators::ASSIGN,
                                   operators::WALRUS,
                                   operators::PLUS,
                                   operators::PLUS_ASSIGN,
                                   operators::MINUS,
                                   operators::MINUS_ASSIGN,
                                   operators::STAR,
                                   operators::STAR_ASSIGN,
                                   operators::SLASH,
                                   operators::SLASH_ASSIGN,
                                   operators::PERCENT,
                                   operators::PERCENT_ASSIGN,
                                   operators::BANG,
                                   operators::AND_MUT,
                                   operators::CARET_MUT,
                                   operators::BW_AND,
                                   operators::BW_AND_ASSIGN,
                                   operators::BW_OR,
                                   operators::BW_OR_ASSIGN,
                                   operators::SHL,
                                   operators::SHL_ASSIGN,
                                   operators::SHR,
                                   operators::SHR_ASSIGN,
                                   operators::NOT,
                                   operators::NOT_ASSIGN,
                                   operators::CARET,
                                   operators::XOR_ASSIGN,
                                   operators::BOOLEAN_AND,
                                   operators::BOOLEAN_OR,
                                   operators::LT,
                                   operators::LT_EQ,
                                   operators::GT,
                                   operators::GT_EQ,
                                   operators::EQ,
                                   operators::NEQ,
                                   operators::ELLIPSIS,
                                   operators::COLON_COLON,
                                   operators::DOT,
                                   operators::DOT_DOT,
                                   operators::DOT_DOT_EQ,
                                   operators::FAT_ARROW,
                                   operators::COMMENT,
                                   operators::MULTILINE_STRING,
                                   operators::NULL_TERMINATED};

    stdx::fixed::hash_map<std::string_view, TokenType, operators.size(), stdx::crc::hash> map;
    for (const auto& op : operators) { map.emplace(op.name, op.type); }
    return map;
}()};

} // namespace

auto max_operator_length() noexcept -> usize {
    return std::ranges::max_element(
               ALL_OPERATORS,
               [](auto a, auto b) -> bool { return a.first.size() < b.first.size(); })
        ->first.size();
}

auto get_operator_opt(std::string_view sv) noexcept -> stdx::option<TokenType> {
    return ALL_OPERATORS.get_opt(sv).materialize();
}

} // namespace ghoti::syntax
