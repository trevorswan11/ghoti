#include "compiler/syntax/precedence.hh"

#include <utility>

#include <stdx/fixed/enum_map.hh>
#include <stdx/option.hh>

#include "compiler/syntax/token_type.hh"

namespace ghoti::syntax {

namespace {

// NOLINTBEGIN
using map_pair = std::pair<token_type_t, stdx::option<binding>>;
constexpr binding assignment_binding{bind_precedence::ASSIGNMENT, true};

constexpr auto ALL_BINDINGS{stdx::fixed::enum_map<token_type_t, stdx::option<binding>>::from(
    stdx::option<binding>{stdx::none},
    map_pair{token_type_t::PLUS, bind_precedence::ADD_SUB},
    map_pair{token_type_t::PLUS_PERCENT, bind_precedence::ADD_SUB},
    map_pair{token_type_t::MINUS, bind_precedence::ADD_SUB},
    map_pair{token_type_t::MINUS_PERCENT, bind_precedence::ADD_SUB},
    map_pair{token_type_t::STAR, bind_precedence::MUL_DIV},
    map_pair{token_type_t::STAR_PERCENT, bind_precedence::MUL_DIV},
    map_pair{token_type_t::SLASH, bind_precedence::MUL_DIV},
    map_pair{token_type_t::PERCENT, bind_precedence::MUL_DIV},
    map_pair{token_type_t::BOOLEAN_AND, bind_precedence::BOOL_AND_OR},
    map_pair{token_type_t::BOOLEAN_OR, bind_precedence::BOOL_AND_OR},
    map_pair{token_type_t::EQ, bind_precedence::BOOL_EQUIV},
    map_pair{token_type_t::NEQ, bind_precedence::BOOL_EQUIV},
    map_pair{token_type_t::LT, bind_precedence::BOOL_LT_GT},
    map_pair{token_type_t::LT_EQ, bind_precedence::BOOL_LT_GT},
    map_pair{token_type_t::GT, bind_precedence::BOOL_LT_GT},
    map_pair{token_type_t::GT_EQ, bind_precedence::BOOL_LT_GT},
    map_pair{token_type_t::BW_AND, bind_precedence::MUL_DIV},
    map_pair{token_type_t::BW_OR, bind_precedence::ADD_SUB},
    map_pair{token_type_t::CARET, bind_precedence::ADD_SUB},
    map_pair{token_type_t::SHR, bind_precedence::MUL_DIV},
    map_pair{token_type_t::SHL, bind_precedence::MUL_DIV},
    map_pair{token_type_t::SHL_PERCENT, bind_precedence::MUL_DIV},
    map_pair{token_type_t::LPAREN, bind_precedence::GROUP_CALL_IDX},
    map_pair{token_type_t::LBRACKET, bind_precedence::GROUP_CALL_IDX},
    map_pair{token_type_t::QUESTION, bind_precedence::GROUP_CALL_IDX},
    map_pair{token_type_t::BANG, bind_precedence::GROUP_CALL_IDX},
    map_pair{token_type_t::DOT_DOT, bind_precedence::RANGE},
    map_pair{token_type_t::DOT_DOT_EQ, bind_precedence::RANGE},
    map_pair{token_type_t::ASSIGN, assignment_binding},
    map_pair{token_type_t::PLUS_ASSIGN, assignment_binding},
    map_pair{token_type_t::PLUS_PERCENT_ASSIGN, assignment_binding},
    map_pair{token_type_t::MINUS_ASSIGN, assignment_binding},
    map_pair{token_type_t::MINUS_PERCENT_ASSIGN, assignment_binding},
    map_pair{token_type_t::STAR_ASSIGN, assignment_binding},
    map_pair{token_type_t::STAR_PERCENT_ASSIGN, assignment_binding},
    map_pair{token_type_t::SLASH_ASSIGN, assignment_binding},
    map_pair{token_type_t::PERCENT_ASSIGN, assignment_binding},
    map_pair{token_type_t::BW_AND_ASSIGN, assignment_binding},
    map_pair{token_type_t::BW_OR_ASSIGN, assignment_binding},
    map_pair{token_type_t::SHL_ASSIGN, assignment_binding},
    map_pair{token_type_t::SHL_PERCENT_ASSIGN, assignment_binding},
    map_pair{token_type_t::SHR_ASSIGN, assignment_binding},
    map_pair{token_type_t::NOT_ASSIGN, assignment_binding},
    map_pair{token_type_t::XOR_ASSIGN, assignment_binding},
    map_pair{token_type_t::DOT, bind_precedence::SCOPE_RESOLUTION},
    map_pair{token_type_t::COLON_COLON, bind_precedence::SCOPE_RESOLUTION},
    map_pair{token_type_t::LBRACE, bind_precedence::INITIALIZATION},
    map_pair{token_type_t::COLON, bind_precedence::LABEL})};
// NOLINTEND

} // namespace

auto binding::try_get_from(token_type_t tt) noexcept -> stdx::option<binding> {
    return ALL_BINDINGS[tt];
}

} // namespace ghoti::syntax
