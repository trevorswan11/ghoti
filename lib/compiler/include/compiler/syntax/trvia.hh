#pragma once

#include <string_view>
#include <vector>

#include <stdx/types.hh>

#include "compiler/syntax/token.hh"

namespace ghoti::syntax {

enum class trivia_kind : u8 {
    WHITESPACE,
    NEWLINE,
    BLANK_LINE,
    LINE_COMMENT,
};

struct trivia_t {
    trivia_kind      kind;
    std::string_view slice;
    usize            line;
    usize            col;
};

struct enriched_token {
    syntax::token_t       token;
    std::vector<trivia_t> leading_trivia;
    std::vector<trivia_t> trailing_trivia;
};

} // namespace ghoti::syntax
