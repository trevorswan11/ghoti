#include "compiler/ast/type.hh"

#include <utility>
#include <vector>

#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/syntax/error.hh"
#include "compiler/syntax/parser.hh"
#include "compiler/syntax/precedence.hh"
#include "compiler/syntax/token_type.hh"

namespace ghoti::ast {

auto explicit_function_type::parse(syntax::parser& parser)
    -> stdx::result<explicit_function_type, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    TRY(parser.expect_peek(syntax::token_type_t::LPAREN));

    // Parse the definition now that we're at the fn token
    std::vector<explicit_type_id> parameter_types;
    bool                          variadic{false};
    if (parser.peek_token_is(syntax::token_type_t::RPAREN)) {
        parser.advance();
    } else {
        // There is no self parameter for types
        while (!parser.peek_token_is(syntax::token_type_t::RPAREN) &&
               !parser.peek_token_is(syntax::token_type_t::END)) {
            if (TRY(try_parse_variadic_fn(parser))) {
                variadic = true;
                break;
            }

            // There are no default values for parameters, and they must be explicitly typed
            auto type{TRY(explicit_type::parse(parser))};
            if (type.is<identifier_expr>()) {
                // noreturn is not allowed for parameters
                if (type.get_token_type() == syntax::token_type_t::NORETURN) {
                    return make_syntax_err("Function parameter types may not be marked `noreturn`",
                                           syntax::error::FN_PARAMETER_IS_NORETURN,
                                           parser.get_location_of(type));
                }
            }

            parameter_types.emplace_back(type);
            if (!parser.peek_token_is(syntax::token_type_t::RPAREN)) {
                TRY(parser.expect_peek(syntax::token_type_t::COMMA));
            }
        }
        TRY(parser.expect_peek(syntax::token_type_t::RPAREN));
    }

    // There must be a return type but there cannot be a block
    TRY(parser.expect_peek(syntax::token_type_t::COLON));
    const auto return_type{TRY(explicit_type::parse(parser))};
    if (parser.peek_token_is(syntax::token_type_t::LBRACE)) {
        return make_syntax_err("Function types may not have a body",
                               syntax::error::EXPLICIT_FN_TYPE_HAS_BODY,
                               start_token);
    }

    return explicit_function_type{.parameter_types      = std::move(parameter_types),
                                  .variadic             = variadic,
                                  .explicit_return_type = return_type};
}

auto explicit_type::parse(syntax::parser& parser)
    -> stdx::result<explicit_type_id, syntax::diagnostic> {
    // Always check for a modifier and advance past it if present
    PROFILE_FUNCTION();
    const auto    modifier_token{parser.get_peek_token()};
    type_modifier modifier{modifier_token};
    if (modifier_token.type == syntax::token_type_t::MUT) {
        parser.advance();
        modifier = TRY(type_modifier::parse(parser, modifier_token));
    } else if (!modifier.is_value()) {
        parser.advance();
    }

    // The array dimension of a type are only present conditionally
    if (parser.peek_token_is(syntax::token_type_t::LBRACKET)) {
        parser.advance();

        auto                      null_terminated{false};
        stdx::option<expr_handle> dimension;
        if (parser.peek_token_is(syntax::token_type_t::NULL_TERMINATED)) {
            parser.advance();
            null_terminated = true;
        } else if (!parser.peek_token_is(syntax::token_type_t::RBRACKET)) {
            parser.advance();
            dimension.emplace(TRY(parser.parse_expression()));

            // The null terminated marker comes after the size for explicitly sized types
            if (parser.peek_token_is(syntax::token_type_t::NULL_TERMINATED)) {
                parser.advance();
                null_terminated = true;
            }
        }
        TRY(parser.expect_peek(syntax::token_type_t::RBRACKET));

        auto mut_elements{false};
        if (parser.peek_token_is(syntax::token_type_t::MUT)) {
            parser.advance();
            mut_elements = true;
        }

        // Arrays are recursively defined
        const auto inner{TRY(explicit_type::parse(parser))};
        return parser.add_type<explicit_array_type>(
            modifier_token, modifier, dimension, null_terminated, mut_elements, inner);
    }

    if (!type_modifier{parser.get_peek_token()}.is_value()) {
        // Don't advance since the parser does it implicitly here (costs two modifier queries)
        const auto inner{TRY(explicit_type::parse(parser))};
        return parser.add_type<explicit_type_id>(modifier_token, modifier, inner);
    }

    // Otherwise the type has to be a 'simple' function or ident
    const auto& peek_token{parser.get_peek_token()};
    if (peek_token.is_valid_ident()) {
        // It's trivial to catch these syntactic errors here
        if (!modifier.is_value()) {
            switch (peek_token.type) {
            case syntax::token_type_t::TYPE_TYPE:
                return make_syntax_err("Explicit `type` type cannot have a modifier",
                                       syntax::error::ILLEGAL_TYPE_TYPE_MODIFIER,
                                       modifier_token);
            case syntax::token_type_t::VOID_TYPE:
                return make_syntax_err("Explicit `void` type cannot have a modifier",
                                       syntax::error::ILLEGAL_VOID_TYPE_MODIFIER,
                                       modifier_token);
            case syntax::token_type_t::NORETURN:
                return make_syntax_err("Explicit `noreturn` type cannot have a modifier",
                                       syntax::error::ILLEGAL_NORETURN_TYPE_MODIFIER,
                                       modifier_token);
            default: break;
            }
        }

        parser.advance();
        // Manually dispatch to prevent weird consumption
        if (parser.peek_token_is(syntax::token_type_t::COLON_COLON) ||
            parser.peek_token_is(syntax::token_type_t::LPAREN)) {
            const auto parsed{TRY(parser.parse_expression(syntax::bind_precedence::TYPE))};
            if (parsed.is<module_access_expr>()) {
                return parser.add_type<module_access_expr>(
                    modifier_token, modifier, parser.get_node<module_access_expr>(*parsed));
            }

            if (parsed.is<dot_expr>()) {
                return parser.add_type<dot_expr>(
                    modifier_token, modifier, parser.get_node<dot_expr>(*parsed));
            }

            if (parsed.is<call_expr>()) {
                return parser.add_type<call_expr>(
                    modifier_token, modifier, parser.get_node<call_expr>(*parsed));
            }

            return make_syntax_err(syntax::error::ILLEGAL_EXPLICIT_TYPE,
                                   parser.get_location_of(*parsed));
        }

        const auto ident{TRY(identifier_expr::parse(parser))};
        return parser.add_type<identifier_expr>(
            modifier_token, modifier, parser.get_node<identifier_expr>(*ident));
    }

    // The inner type is limited to functions and user-defined types
    const auto type_start{parser.get_current_token()};
    if (parser.peek_token_is(syntax::token_type_t::FUNCTION)) {
        parser.advance();
        const auto fn_type{TRY(explicit_function_type::parse(parser))};
        if (!(modifier.is_value() || modifier.is_ptr())) {
            return make_syntax_err("Functions types may only be values or pointers",
                                   syntax::error::ILLEGAL_FUNCTION_TYPE_MODIFIER,
                                   type_start);
        }

        // Function types cannot have bodies
        return parser.add_type<explicit_function_type>(modifier_token, modifier, fn_type);
    }

    // The user-defined types can be handled by parsing any expression and verifying it
    parser.advance();
    if (parser.current_token_is(syntax::token_type_t::END)) {
        return make_syntax_err(syntax::error::MISSING_EXPLICIT_TYPE, type_start);
    }

    stdx::option<explicit_type_id> id;
    switch (const auto user{TRY(parser.parse_expression())}; user->get_kind()) {
    case node_kind::STRUCT_EXPRESSION:
        id.emplace(parser.add_type<struct_expr>(
            modifier_token, modifier, parser.get_node<struct_expr>(*user)));
        break;
    case node_kind::ENUM_EXPRESSION:
        id.emplace(parser.add_type<enum_expr>(
            modifier_token, modifier, parser.get_node<enum_expr>(*user)));
        break;
    case node_kind::UNION_EXPRESSION:
        id.emplace(parser.add_type<union_expr>(
            modifier_token, modifier, parser.get_node<union_expr>(*user)));
        break;
    default: break;
    }

    if (id) {
        if (!modifier.is_value()) {
            return make_syntax_err(
                "User-defined types can only be defined with non-modified aliases",
                syntax::error::ILLEGAL_USING_ALIAS_WITH_MODIFIERS,
                type_start);
        }
        return *id;
    }
    return make_syntax_err(syntax::error::ILLEGAL_EXPLICIT_TYPE, type_start);
}

namespace {

// Parses the explicit type if present and checks for an upcoming assignment for init
[[nodiscard]] auto parse_type_and_initializer(syntax::parser& parser)
    -> stdx::result<std::pair<stdx::option<explicit_type_id>, bool>, syntax::diagnostic> {
    if (parser.peek_token_is(syntax::token_type_t::WALRUS)) {
        parser.advance();
        return std::pair{stdx::none, true};
    }

    if (parser.peek_token_is(syntax::token_type_t::COLON)) {
        parser.advance();
        const auto explicit_type{TRY(explicit_type::parse(parser))};
        if (parser.peek_token_is(syntax::token_type_t::ASSIGN)) {
            parser.advance();
            return std::pair{explicit_type, true};
        }
        return std::pair{explicit_type, false};
    }

    return stdx::err{parser.peek_error(syntax::token_type_t::COLON)};
}

} // namespace

auto explicit_type::parse_opt_init(syntax::parser& parser)
    -> stdx::result<std::pair<stdx::option<explicit_type_id>, bool>, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto [type, initialized]{TRY(parse_type_and_initializer(parser))};

    // Advance again to prepare for rhs
    if (initialized) { parser.advance(); }
    return std::pair{type, initialized};
}

} // namespace ghoti::ast
