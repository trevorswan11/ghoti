#include "compiler/syntax/parser.hh"

#include <cctype>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>
#include <stdx/enum.hh>
#include <stdx/fixed/enum_map.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/error.hh"
#include "compiler/syntax/keywords.hh"
#include "compiler/syntax/precedence.hh"
#include "compiler/syntax/token.hh"
#include "compiler/syntax/token_type.hh"
#include "support/diagnostic.hh"

namespace ghoti::syntax {

auto parser::reset(std::string_view input) noexcept -> void {
    ast_.reset();
    input_ = input;
    lexer_.reset(input);
    advance(2);
}

namespace {

// Skip comments here so no downstream code has to special-case `token_type_t::COMMENT`.
[[nodiscard]] auto next_significant_token(lexer& lex) noexcept -> token_t {
    auto token{lex.advance()};
    while (token.type == token_type_t::COMMENT) { token = lex.advance(); }
    return token;
}

} // namespace

auto parser::advance(u8 times) noexcept -> const token_t& {
    for (u8 i = 0; i < times; ++i) {
        current_token_ = peek_token_;
        peek_token_    = next_significant_token(lexer_);
    }
    return current_token_;
}

auto parser::consume(ast::AST& ast) -> diagnostics {
    PROFILE_FUNCTION();
    reset(input_);
    ast.clear();
    ast_.emplace(ast);

    diagnostics diagnostics;

    while (!current_token_is(token_type_t::END)) {
        // Advance through any amount of semicolons
        const auto skip = [](token_type_t tt) -> bool { return tt == token_type_t::SEMICOLON; };
        if (skip(current_token_.type)) { while (skip(advance().type)); } // NOLINT
        if (current_token_is(token_type_t::END)) { break; }

        auto stmt{parse_statement()};
        if (stmt) {
            ast.add_root(**stmt);
        } else {
            diagnostics.emplace_back(std::move(stmt.error()));

            // Errors should advance up to next logical end to prevent useless errors
            const auto stop_condition = [](token_type_t tt) -> bool {
                switch (tt) {
                case token_type_t::RBRACE:
                case token_type_t::SEMICOLON:
                case token_type_t::END:       return true;
                default:                      return false;
                }
            };
            while (!stop_condition(advance().type)); // NOLINT
        }
        advance();
    }

    return diagnostics;
}

auto parser::expect_peek(token_type_t expected) -> stdx::result<void, diagnostic> {
    if (peek_token_is(expected)) {
        advance();
        return {};
    }
    return stdx::err{peek_error(expected)};
}

auto parser::expect_semicolon() -> stdx::result<void, diagnostic> {
    using token_type_t::SEMICOLON;
    if (current_token_is(SEMICOLON)) { return {}; }
    return expect_peek(SEMICOLON);
}

auto parser::peek_error(token_type_t expected) -> diagnostic {
    return diagnostic{fmt::format("Expected token {}, found {}",
                                  magic_enum::enum_name(expected),
                                  magic_enum::enum_name(peek_token_.type)),
                      error::UNEXPECTED_TOKEN,
                      peek_token_};
}

auto parser::get_current_precedence() const noexcept
    -> std::pair<bind_precedence, stdx::option<binding>> {
    return binding::try_get_from(current_token_.type)
        .transform(
            [](const auto& b) -> auto { return std::pair{b.precedence, stdx::option<binding>{b}}; })
        .value_or(std::pair{bind_precedence::LOWEST, stdx::none});
}

auto parser::get_peek_precedence() const noexcept
    -> std::pair<bind_precedence, stdx::option<binding>> {
    return binding::try_get_from(peek_token_.type)
        .transform(
            [](const auto& b) -> auto { return std::pair{b.precedence, stdx::option<binding>{b}}; })
        .value_or(std::pair{bind_precedence::LOWEST, stdx::none});
}

auto parser::parse_statement(semicolon_behavior behavior)
    -> stdx::result<ast::stmt_handle, diagnostic> {
    // Not all decls are public so the condition needs to be rechecked
    PROFILE_FUNCTION();
    if (current_token_.type == token_type_t::PUBLIC) {
        switch (peek_token_.type) {
        case token_type_t::IMPORT: return ast::import_stmt::parse(*this);
        case token_type_t::USING:  return ast::using_stmt::parse(*this);
        default:                   return ast::decl_stmt::parse(*this);
        }
    } else if (current_token_.is_decl_token()) {
        return ast::decl_stmt::parse(*this);
    }

    switch (current_token_.type) {
    case token_type_t::LBRACE:     return ast::block_stmt::parse(*this);
    case token_type_t::BREAK:      return ast::break_stmt::parse(*this);
    case token_type_t::CONTINUE:   return ast::continue_stmt::parse(*this);
    case token_type_t::DEFER:      return ast::defer_stmt::parse(*this);
    case token_type_t::UNDERSCORE: return ast::discard_stmt::parse(*this);
    case token_type_t::IMPORT:     return ast::import_stmt::parse(*this);
    case token_type_t::RETURN:     return ast::return_stmt::parse(*this);
    case token_type_t::TEST:       return ast::test_stmt::parse(*this);
    case token_type_t::USING:      return ast::using_stmt::parse(*this);
    default:                       return ast::expr_stmt::parse(*this, behavior);
    }
}

auto parser::parse_expression(bind_precedence precedence)
    -> stdx::result<ast::expr_handle, diagnostic> {
    PROFILE_FUNCTION();
    if (current_token_is(token_type_t::END)) {
        return make_syntax_err(error::END_OF_TOKEN_STREAM, current_token_);
    }

    const depth_guard guard{expr_depth_};
    if (expr_depth_ > MAX_EXPRESSION_DEPTH) {
        return make_syntax_err(
            "Expression nested too deeply", error::EXPRESSION_NESTED_TOO_DEEPLY, current_token_);
    }

    const auto prefix{get_prefix_fn_opt(current_token_.type)};
    if (!prefix) {
        // The slice's leading char reliably tells which lexer failure produced ILLEGAL.
        if (current_token_is(token_type_t::ILLEGAL) && !current_token_.slice.empty()) {
            switch (current_token_.slice.front()) {
            case '"':
                return make_syntax_err(
                    "Unterminated string literal", error::UNTERMINATED_STRING, current_token_);
            case '\'':
                return make_syntax_err("Invalid or unterminated character literal",
                                       error::INVALID_CHARACTER_LITERAL,
                                       current_token_);
            default:
                if (std::isdigit(static_cast<u8>(current_token_.slice.front()))) {
                    return make_syntax_err(
                        "Invalid numeric literal", error::INVALID_NUMBER_LITERAL, current_token_);
                }
                break;
            }
        }
        return make_syntax_err(fmt::format("No prefix parse function for {}({}) found",
                                           magic_enum::enum_name(current_token_.type),
                                           current_token_.slice),
                               error::MISSING_PREFIX_PARSER,
                               current_token_);
    }
    auto lhs_expression{TRY((*prefix)(*this))};

    while (!peek_token_is(token_type_t::SEMICOLON) && precedence < get_peek_precedence().first) {
        const auto infix{get_poll_infix_fn_opt(peek_token_.type)};
        if (!infix) { break; }
        advance();
        lhs_expression = TRY((*infix)(*this, lhs_expression));
    }

    return lhs_expression;
}

[[nodiscard]] auto parser::parse_restricted_statement(error error, semicolon_behavior behavior)
    -> stdx::result<ast::stmt_handle, diagnostic> {
    auto clause{TRY(parse_statement(behavior))};

    // The clause can only be a jump, block, or expression statement
    if (!clause.any<ast::expr_stmt,
                    ast::break_stmt,
                    ast::continue_stmt,
                    ast::return_stmt,
                    ast::block_stmt>()) {
        return make_syntax_err(error, ast_->location_of(*clause));
    }
    return clause;
}

[[nodiscard]] auto parser::try_parse_restricted_alternate(error error, semicolon_behavior behavior)
    -> stdx::result<stdx::option<ast::stmt_handle>, diagnostic> {
    if (peek_token_is(token_type_t::ELSE)) {
        // Advance twice to actually look at the statement's first token
        advance(2);
        return TRY(parse_restricted_statement(error, behavior));
    }
    return stdx::none;
}

namespace {

constexpr auto PREFIX_FNS = [] -> auto {
    stdx::fixed::enum_map<token_type_t, parser::prefix_fn> fns;

    fns[token_type_t::IDENT]            = ast::identifier_expr::parse;
    fns[token_type_t::U8]               = ast::u8_expr::parse;
    fns[token_type_t::F32]              = ast::f32_expr::parse;
    fns[token_type_t::F64]              = ast::f64_expr::parse;
    fns[token_type_t::BANG]             = ast::unary_expr::parse;
    fns[token_type_t::NOT]              = ast::unary_expr::parse;
    fns[token_type_t::MINUS]            = ast::unary_expr::parse;
    fns[token_type_t::PLUS]             = ast::unary_expr::parse;
    fns[token_type_t::STAR]             = ast::dereference_expr::parse;
    fns[token_type_t::BW_AND]           = ast::reference_expr::parse;
    fns[token_type_t::AND_MUT]          = ast::reference_expr::parse;
    fns[token_type_t::CARET]            = ast::address_of_expr::parse;
    fns[token_type_t::CARET_MUT]        = ast::address_of_expr::parse;
    fns[token_type_t::DOT]              = ast::implicit_access_expr::parse;
    fns[token_type_t::BOOLEAN_TRUE]     = ast::bool_expr::parse;
    fns[token_type_t::BOOLEAN_FALSE]    = ast::bool_expr::parse;
    fns[token_type_t::UNDEFINED]        = ast::undefined_expr::parse;
    fns[token_type_t::NULLPTR]          = ast::nullptr_expr::parse;
    fns[token_type_t::UNREACHABLE]      = ast::unreachable_expr::parse;
    fns[token_type_t::LBRACE]           = ast::void_expr::parse;
    fns[token_type_t::STRING]           = ast::string_expr::parse;
    fns[token_type_t::MULTILINE_STRING] = ast::string_expr::parse;
    fns[token_type_t::LPAREN]           = ast::grouped_expr::parse;
    fns[token_type_t::IF]               = ast::if_expr::parse;
    fns[token_type_t::FUNCTION]         = ast::function_expr::parse;
    fns[token_type_t::MOVE]             = ast::parse_move_function_expr;
    fns[token_type_t::NAKED]            = ast::parse_naked_function_expr;
    fns[token_type_t::STRUCT]           = ast::struct_expr::parse;
    fns[token_type_t::UNION]            = ast::union_expr::parse;
    fns[token_type_t::EXTERN]           = ast::parse_modified_struct_or_union;
    fns[token_type_t::PACKED]           = ast::parse_modified_struct_or_union;
    fns[token_type_t::ENUM]             = ast::enum_expr::parse;
    fns[token_type_t::MATCH]            = ast::match_expr::parse;
    fns[token_type_t::ASM]              = ast::asm_expr::parse;
    fns[token_type_t::LBRACKET]         = ast::array_expr::parse;
    fns[token_type_t::FOR]              = ast::for_loop_expr::parse;
    fns[token_type_t::WHILE]            = ast::while_loop_expr::parse;
    fns[token_type_t::DO]               = ast::do_while_loop_expr::parse;
    fns[token_type_t::LOOP]             = ast::infinite_loop_expr::parse;

    for (const auto tt : stdx::enum_range<token_type_t::INT_2, token_type_t::UZINT_16>()) {
        using token_type::integer_category;
        switch (token_type::to_int_category(tt)) {
        case integer_category::SIGNED_BASE:   fns[tt] = ast::i32_expr::parse; break;
        case integer_category::SIGNED_WIDE:   fns[tt] = ast::i64_expr::parse; break;
        case integer_category::SIGNED_SIZE:   fns[tt] = ast::isize_expr::parse; break;
        case integer_category::UNSIGNED_BASE: fns[tt] = ast::u32_expr::parse; break;
        case integer_category::UNSIGNED_WIDE: fns[tt] = ast::u64_expr::parse; break;
        case integer_category::UNSIGNED_SIZE: fns[tt] = ast::usize_expr::parse; break;
        }
    }

    for (const auto tt : ALL_PRIMITIVES) { fns[tt] = ast::identifier_expr::parse; }
    for (const auto tt : builtins::ALL_TOKEN_TYPES) { fns[tt] = ast::identifier_expr::parse; }

    return fns;
}();

constexpr auto INFIX_FNS = [] -> auto {
    stdx::fixed::enum_map<token_type_t, parser::infix_fn> fns;

    fns[token_type_t::PLUS]           = ast::binary_expr::parse;
    fns[token_type_t::MINUS]          = ast::binary_expr::parse;
    fns[token_type_t::STAR]           = ast::binary_expr::parse;
    fns[token_type_t::SLASH]          = ast::binary_expr::parse;
    fns[token_type_t::PERCENT]        = ast::binary_expr::parse;
    fns[token_type_t::LT]             = ast::binary_expr::parse;
    fns[token_type_t::LT_EQ]          = ast::binary_expr::parse;
    fns[token_type_t::GT]             = ast::binary_expr::parse;
    fns[token_type_t::GT_EQ]          = ast::binary_expr::parse;
    fns[token_type_t::EQ]             = ast::binary_expr::parse;
    fns[token_type_t::NEQ]            = ast::binary_expr::parse;
    fns[token_type_t::BOOLEAN_AND]    = ast::binary_expr::parse;
    fns[token_type_t::BOOLEAN_OR]     = ast::binary_expr::parse;
    fns[token_type_t::BW_AND]         = ast::binary_expr::parse;
    fns[token_type_t::BW_OR]          = ast::binary_expr::parse;
    fns[token_type_t::CARET]          = ast::binary_expr::parse;
    fns[token_type_t::SHR]            = ast::binary_expr::parse;
    fns[token_type_t::SHL]            = ast::binary_expr::parse;
    fns[token_type_t::DOT]            = ast::dot_expr::parse;
    fns[token_type_t::DOT_DOT]        = ast::range_expr::parse;
    fns[token_type_t::DOT_DOT_EQ]     = ast::range_expr::parse;
    fns[token_type_t::LPAREN]         = ast::call_expr::parse;
    fns[token_type_t::LBRACKET]       = ast::index_expr::parse;
    fns[token_type_t::LBRACE]         = ast::initializer_expr::parse;
    fns[token_type_t::ASSIGN]         = ast::assignment_expr::parse;
    fns[token_type_t::PLUS_ASSIGN]    = ast::assignment_expr::parse;
    fns[token_type_t::MINUS_ASSIGN]   = ast::assignment_expr::parse;
    fns[token_type_t::STAR_ASSIGN]    = ast::assignment_expr::parse;
    fns[token_type_t::SLASH_ASSIGN]   = ast::assignment_expr::parse;
    fns[token_type_t::PERCENT_ASSIGN] = ast::assignment_expr::parse;
    fns[token_type_t::BW_AND_ASSIGN]  = ast::assignment_expr::parse;
    fns[token_type_t::BW_OR_ASSIGN]   = ast::assignment_expr::parse;
    fns[token_type_t::SHL_ASSIGN]     = ast::assignment_expr::parse;
    fns[token_type_t::SHR_ASSIGN]     = ast::assignment_expr::parse;
    fns[token_type_t::NOT_ASSIGN]     = ast::assignment_expr::parse;
    fns[token_type_t::XOR_ASSIGN]     = ast::assignment_expr::parse;
    fns[token_type_t::COLON_COLON]    = ast::module_access_expr::parse;
    fns[token_type_t::COLON]          = ast::label_expr::parse;

    return fns;
}();

} // namespace

auto parser::get_prefix_fn_opt(token_type_t tt) noexcept -> stdx::option<prefix_fn> {
    return PREFIX_FNS.get_opt(tt);
}

auto parser::get_poll_infix_fn_opt(token_type_t tt) noexcept -> stdx::option<infix_fn> {
    return INFIX_FNS.get_opt(tt);
}

auto parser::get_location_of(ast::node_id id) -> source_location { return ast_->location_of(id); }

auto parser::get_location_of(ast::explicit_type_id id) -> source_location {
    return ast_->location_of(id);
}

} // namespace ghoti::syntax
