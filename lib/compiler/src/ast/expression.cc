#include "compiler/ast/expression.hh"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/type.hh"
#include "compiler/syntax/error.hh"
#include "compiler/syntax/parser.hh"
#include "compiler/syntax/precedence.hh"
#include "compiler/syntax/token.hh"
#include "compiler/syntax/token_type.hh"

namespace ghoti::ast {

auto array_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    auto       null_terminated{false};

    stdx::option<expr_handle> size;
    if (!parser.peek_token_is(syntax::token_type_t::RBRACKET)) {
        parser.advance();
        if (!parser.current_token_is(syntax::token_type_t::UNDERSCORE)) {
            size.emplace(TRY(parser.parse_expression()));
        }

        // The null terminated marker comes after the size for explicitly sized types
        if (parser.peek_token_is(syntax::token_type_t::NULL_TERMINATED)) {
            parser.advance();
            null_terminated = true;
        }
    } else {
        // There needs to be a token for the size for array literals
        return make_syntax_err(
            "Array literals must be initialized with an implicit or explicit size",
            syntax::error::MISSING_ARRAY_SIZE_TOKEN,
            start_token);
    }

    TRY(parser.expect_peek(syntax::token_type_t::RBRACKET));

    auto mut_elements{false};
    if (parser.peek_token_is(syntax::token_type_t::MUT)) {
        parser.advance();
        mut_elements = true;
    }

    const auto item_type{TRY(explicit_type::parse(parser, true))};
    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));

    // Current token is either the LBRACE at the start or a comma before parsing
    std::vector<expr_handle> items;
    while (!parser.peek_token_is(syntax::token_type_t::RBRACE) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        parser.advance();
        items.emplace_back(TRY(parser.parse_expression()));
        if (!parser.peek_token_is(syntax::token_type_t::RBRACE)) {
            TRY(parser.expect_peek(syntax::token_type_t::COMMA));
        }
    }

    TRY(parser.expect_peek(syntax::token_type_t::RBRACE));
    return parser.add_expr<array_expr>(
        start_token, size, null_terminated, mut_elements, item_type, std::move(items));
}

namespace {

[[nodiscard]] auto map_asm_option(std::string_view name) noexcept
    -> stdx::option<asm_expr::option> {
    if (name == "volatile") { return asm_expr::option::VOLATILE; }
    if (name == "noreturn") { return asm_expr::option::NORETURN; }
    if (name == "intel") { return asm_expr::option::INTEL; }
    if (name == "att") { return asm_expr::option::ATT; }
    if (name == "align_stack") { return asm_expr::option::ALIGN_STACK; }
    return stdx::none;
}

// Parses one `"<constraint>" = <expr>` / `"<constraint>" = _` operand entry
[[nodiscard]] auto parse_asm_operand(syntax::parser& parser)
    -> stdx::result<asm_expr::operand, syntax::diagnostic> {
    if (!parser.peek_token_is(syntax::token_type_t::STRING)) {
        return make_syntax_err("An asm operand must begin with a constraint string literal",
                               syntax::error::ASM_MALFORMED_OPERAND,
                               parser.get_peek_token());
    }
    parser.advance();
    const string_handle constraint{TRY(string_expr::parse(parser))};
    TRY(parser.expect_peek(syntax::token_type_t::ASSIGN));

    stdx::option<expr_handle> value;
    if (parser.peek_token_is(syntax::token_type_t::UNDERSCORE)) {
        parser.advance();
    } else {
        parser.advance();
        value.emplace(TRY(parser.parse_expression()));
    }
    return asm_expr::operand{constraint, value};
}

template <typename Fn>
[[nodiscard]] auto parse_asm_paren_list(syntax::parser& parser, Fn&& element)
    -> stdx::result<void, syntax::diagnostic> {
    TRY(parser.expect_peek(syntax::token_type_t::LPAREN));
    while (!parser.peek_token_is(syntax::token_type_t::RPAREN) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        TRY(element());
        if (!parser.peek_token_is(syntax::token_type_t::RPAREN)) {
            TRY(parser.expect_peek(syntax::token_type_t::COMMA));
        }
    }
    return parser.expect_peek(syntax::token_type_t::RPAREN);
}

} // namespace

auto asm_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};

    // An optional explicit result type precedes the brace body: `asm u32 { ... }`
    stdx::option<explicit_type_id> result_type;
    if (!parser.peek_token_is(syntax::token_type_t::LBRACE)) {
        result_type.emplace(TRY(explicit_type::parse(parser, true)));
    }
    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));

    stdx::option<string_handle> tmpl;
    std::vector<operand>        outputs;
    std::vector<operand>        inputs;
    std::vector<string_handle>  clobbers;
    std::vector<option>         options;
    bool                        seen_template{false}, seen_outputs{false}, seen_inputs{false};
    bool                        seen_clobbers{false}, seen_options{false};

    const auto reject_duplicate =
        [&](bool& flag, const syntax::token_t& key) -> stdx::result<void, syntax::diagnostic> {
        if (flag) {
            return make_syntax_err(fmt::format("Duplicate asm clause '{}'", key.slice),
                                   syntax::error::ASM_DUPLICATE_CLAUSE,
                                   key);
        }
        flag = true;
        return {};
    };

    while (!parser.peek_token_is(syntax::token_type_t::RBRACE) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        TRY(parser.expect_peek(syntax::token_type_t::IDENT));
        const auto key_token{parser.get_current_token()};
        const auto key{key_token.slice};
        TRY(parser.expect_peek(syntax::token_type_t::COLON));

        if (key == "template") {
            TRY(reject_duplicate(seen_template, key_token));
            if (!parser.peek_token_is(syntax::token_type_t::STRING) &&
                !parser.peek_token_is(syntax::token_type_t::MULTILINE_STRING)) {
                return make_syntax_err("The asm 'template' clause requires a string literal",
                                       syntax::error::ASM_MISSING_TEMPLATE,
                                       parser.get_peek_token());
            }
            parser.advance();
            tmpl.emplace(string_handle{TRY(string_expr::parse(parser))});
        } else if (key == "outputs") {
            TRY(reject_duplicate(seen_outputs, key_token));
            TRY(parse_asm_paren_list(parser, [&] -> stdx::result<void, syntax::diagnostic> {
                outputs.emplace_back(TRY(parse_asm_operand(parser)));
                return {};
            }));
        } else if (key == "inputs") {
            TRY(reject_duplicate(seen_inputs, key_token));
            TRY(parse_asm_paren_list(parser, [&] -> stdx::result<void, syntax::diagnostic> {
                inputs.emplace_back(TRY(parse_asm_operand(parser)));
                return {};
            }));
        } else if (key == "clobbers") {
            TRY(reject_duplicate(seen_clobbers, key_token));
            TRY(parse_asm_paren_list(parser, [&] -> stdx::result<void, syntax::diagnostic> {
                if (!parser.peek_token_is(syntax::token_type_t::STRING)) {
                    return make_syntax_err("An asm clobber must be a string literal",
                                           syntax::error::ASM_MALFORMED_OPERAND,
                                           parser.get_peek_token());
                }
                parser.advance();
                clobbers.emplace_back(string_handle{TRY(string_expr::parse(parser))});
                return {};
            }));
        } else if (key == "options") {
            TRY(reject_duplicate(seen_options, key_token));
            TRY(parse_asm_paren_list(parser, [&] -> stdx::result<void, syntax::diagnostic> {
                // Some (`volatile`, `noreturn`) are reserved keywords
                parser.advance();
                const auto opt_token{parser.get_current_token()};
                const auto opt{map_asm_option(opt_token.slice)};
                if (!opt) {
                    return make_syntax_err(fmt::format("Unknown asm option '{}'", opt_token.slice),
                                           syntax::error::ASM_UNKNOWN_OPTION,
                                           opt_token);
                }
                options.emplace_back(*opt);
                return {};
            }));
        } else {
            return make_syntax_err(fmt::format("Unknown asm clause '{}'", key),
                                   syntax::error::ASM_UNKNOWN_CLAUSE,
                                   key_token);
        }

        if (!parser.peek_token_is(syntax::token_type_t::RBRACE)) {
            TRY(parser.expect_peek(syntax::token_type_t::COMMA));
        }
    }
    TRY(parser.expect_peek(syntax::token_type_t::RBRACE));

    if (!tmpl) {
        return make_syntax_err("An asm block requires a 'template' clause",
                               syntax::error::ASM_MISSING_TEMPLATE,
                               start_token);
    }

    return parser.add_expr<asm_expr>(start_token,
                                     *tmpl,
                                     result_type,
                                     std::move(outputs),
                                     std::move(inputs),
                                     std::move(clobbers),
                                     std::move(options));
}

auto call_expr::parse(syntax::parser& parser, expr_handle function)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto            start_token{parser.get_current_token()};
    std::vector<argument> arguments;
    // Guaranteed to roll back if there is an error
    const auto parse_expr_unsuccessful = [&] -> bool {
        // Try an expression first to prevent ambiguity between reference operators
        syntax::parser::transaction transaction{parser};
        parser.advance();
        if (auto expr{parser.parse_expression()}) {
            transaction.commit();
            arguments.emplace_back(*expr);
            return false;
        }
        return true;
    };

    while (!parser.peek_token_is(syntax::token_type_t::RPAREN) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        if (parser.peek_token_is(syntax::token_type_t::COMMA)) {
            return make_syntax_err("A comma implies an argument but none were found",
                                   syntax::error::COMMA_WITH_MISSING_CALL_ARGUMENT,
                                   parser.get_peek_token());
        }

        // Advance cannot be called here since explicit type relies on peek, not current
        if (parse_expr_unsuccessful()) {
            arguments.emplace_back(TRY(explicit_type::parse(parser)));
        }
        if (!parser.peek_token_is(syntax::token_type_t::RPAREN)) {
            TRY(parser.expect_peek(syntax::token_type_t::COMMA));
        }
    }
    TRY(parser.expect_peek(syntax::token_type_t::RPAREN));

    return parser.add_expr<call_expr>(start_token, function, std::move(arguments));
}

auto do_while_loop_expr::parse(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));

    const block_handle block{TRY(block_stmt::parse(parser))};
    TRY(parser.expect_peek(syntax::token_type_t::WHILE));
    TRY(parser.expect_peek(syntax::token_type_t::LPAREN));

    if (parser.peek_token_is(syntax::token_type_t::RPAREN)) {
        return make_syntax_err("While loops must have a condition",
                               syntax::error::WHILE_MISSING_CONDITION,
                               parser.get_current_token());
    }
    parser.advance();

    // There's no continuation or non break clause so this is easy :)
    const auto condition{TRY(parser.parse_expression())};
    TRY(parser.expect_peek(syntax::token_type_t::RPAREN));
    return parser.add_expr<do_while_loop_expr>(start_token, block, condition);
}

namespace {

[[nodiscard]] auto deconstruct_member(syntax::parser& parser, stmt_handle member)
    -> stdx::result<member_handle, syntax::diagnostic> {
    switch (member->get_kind()) {
    case node_kind::DECL_STATEMENT:
    case node_kind::USING_STATEMENT:
    case node_kind::IMPORT_STATEMENT: return member_handle{member};
    default:
        return make_syntax_err("Members must be declarations, `using` aliases, or imports",
                               syntax::error::INVALID_MEMBER,
                               parser.get_location_of(*member));
    }
}

// Returns an actual value only if a terminal condition was found
[[nodiscard]] auto validate_member_decl(syntax::parser& parser, member_handle member) noexcept
    -> stdx::option<std::string_view> {
    return parser.get_ast()[*member].visit(
        [](const decl_stmt& decl) -> stdx::option<std::string_view> {
            // Members that violate this wouldn't be usable with C
            if (decl.has_modifier(decl_modifiers::EXTERN) ||
                decl.has_modifier(decl_modifiers::EXPORT)) {
                return "Member declarations may neither be marked extern nor export";
            }
            return stdx::none;
        },
        [](const auto&) -> stdx::option<std::string_view> { return stdx::none; });
}

[[nodiscard]] auto parse_members(syntax::parser& parser)
    -> stdx::result<member_list, syntax::diagnostic> {
    member_list members;
    while (!parser.peek_token_is(syntax::token_type_t::RBRACE) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        parser.advance();
        auto parsed_member{TRY(parser.parse_statement())};

        // Downcast the parsed member into the specific member variant and check
        const auto member{TRY(deconstruct_member(parser, parsed_member))};
        if (const auto err_msg{validate_member_decl(parser, member)}) {
            return make_syntax_err(std::string{*err_msg},
                                   syntax::error::INVALID_MEMBER,
                                   parser.get_location_of(*member));
        }
        members.emplace_back(member);
    }
    return members;
}

} // namespace

auto enum_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};

    stdx::option<identifier_handle> underlying;
    if (parser.peek_token_is(syntax::token_type_t::COLON)) {
        parser.advance(2);
        underlying.emplace(TRY(identifier_expr::parse(parser)));
    }
    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));

    bool                     non_exhaustive{false};
    std::vector<enumeration> enumerations;
    while (!parser.peek_token_is(syntax::token_type_t::RBRACE) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        if (parser.get_peek_token().is_member_token()) { break; }

        if (parser.peek_token_is(syntax::token_type_t::UNDERSCORE)) {
            parser.advance();
            if (parser.peek_token_is(syntax::token_type_t::COMMA)) { parser.advance(); }

            // The non exhaustive marker must be the final 'enumeration'
            if (parser.get_peek_token().is_member_token() ||
                parser.peek_token_is(syntax::token_type_t::RBRACE)) {
                non_exhaustive = true;
                break;
            }

            return make_syntax_err(
                "The underscore in non-exhaustive enums must be the last enumeration",
                syntax::error::ILLEGAL_NON_EXHAUSTIVE_ENUM,
                parser.get_current_token());
        }

        TRY(parser.expect_peek(syntax::token_type_t::IDENT));
        const identifier_handle ident{TRY(identifier_expr::parse(parser))};

        stdx::option<expr_handle> value;
        if (parser.peek_token_is(syntax::token_type_t::ASSIGN)) {
            parser.advance(2);
            value.emplace(TRY(parser.parse_expression()));
        }
        enumerations.emplace_back(ident, value);

        // No comma means that its the end or that there is a decl list starting
        if (!parser.peek_token_is(syntax::token_type_t::COMMA)) { break; }
        parser.advance();
    }

    auto members{TRY(parse_members(parser))};
    TRY(parser.expect_peek(syntax::token_type_t::RBRACE));

    // Validate here so that there aren't 3 errors spawning from an empty enum with decls
    if (!non_exhaustive && enumerations.empty()) {
        return make_syntax_err("Enums must be declared with at least one enumeration",
                               syntax::error::EMPTY_ENUM,
                               start_token);
    }
    return parser.add_expr<enum_expr>(
        start_token, underlying, std::move(enumerations), non_exhaustive, std::move(members));
}

auto for_loop_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};

    // Iterables have to be surrounded by parentheses
    TRY(parser.expect_peek(syntax::token_type_t::LPAREN));
    if (parser.peek_token_is(syntax::token_type_t::RPAREN)) {
        return make_syntax_err("For loops must contain at least one iterable",
                               syntax::error::FOR_MISSING_ITERABLES,
                               start_token);
    }

    std::vector<expr_handle> iterables;
    while (!parser.peek_token_is(syntax::token_type_t::RPAREN) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        parser.advance();

        const auto iterable{TRY(parser.parse_expression())};
        iterables.emplace_back(iterable);

        if (!parser.peek_token_is(syntax::token_type_t::RPAREN)) {
            TRY(parser.expect_peek(syntax::token_type_t::COMMA));
        }
    }

    TRY(parser.expect_peek(syntax::token_type_t::RPAREN));
    if (!parser.peek_token_is(syntax::token_type_t::BW_OR)) {
        return make_syntax_err("For loops must contain the same number of captures iterables, "
                               "which can be discarded with an underscore",
                               syntax::error::FOR_ITERABLE_CAPTURE_MISMATCH,
                               start_token);
    }

    // Captures take on something similar to zig's capture syntax
    std::vector<capture> captures;
    TRY(parser.expect_peek(syntax::token_type_t::BW_OR));
    while (!parser.peek_token_is(syntax::token_type_t::BW_OR) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        parser.advance();
        if (parser.current_token_is(syntax::token_type_t::UNDERSCORE)) {
            const auto discarded{parser.add_node<discardable_ident_handle, ast::discarded>(
                parser.get_current_token())};
            captures.emplace_back(type_modifier{}, discarded);
        } else {
            // Always check for a modifier and advance past it if present
            const type_modifier modifier{parser.get_current_token()};
            if (!modifier.is_value()) { parser.advance(); }

            const identifier_handle capture{TRY(identifier_expr::parse(parser))};
            captures.emplace_back(modifier, capture);
        }

        if (!parser.peek_token_is(syntax::token_type_t::BW_OR)) {
            TRY(parser.expect_peek(syntax::token_type_t::COMMA));
        }
    }
    TRY(parser.expect_peek(syntax::token_type_t::BW_OR));

    // Loops must have a well formed block and may have an alternate in non-break cases
    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));
    const block_handle block{TRY(block_stmt::parse(parser))};
    const auto         non_break{
        TRY(parser.try_parse_restricted_alternate(syntax::error::ILLEGAL_LOOP_NON_BREAK))};

    // The number of captures must align with the number of iterables
    if (captures.size() != iterables.size()) {
        return make_syntax_err("The number of for loop captures must match the number of iterables",
                               syntax::error::FOR_ITERABLE_CAPTURE_MISMATCH,
                               start_token);
    }

    return parser.add_expr<for_loop_expr>(
        start_token, std::move(iterables), std::move(captures), block, non_break);
}

// Variadic must be handled first and should break the enclosing loop
auto try_parse_variadic_fn(syntax::parser& parser) -> stdx::result<bool, syntax::diagnostic> {
    bool is_variadic{false};
    if (parser.peek_token_is(syntax::token_type_t::ELLIPSIS)) {
        parser.advance();
        is_variadic = true;
        if (!parser.peek_token_is(syntax::token_type_t::RPAREN)) {
            TRY(parser.expect_peek(syntax::token_type_t::COMMA));
        }
    }
    return is_variadic;
}

auto parse_move_function_expr(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    if (!parser.peek_token_is(syntax::token_type_t::FUNCTION)) {
        return make_syntax_err("'move' may only appear directly before 'fn'",
                               syntax::error::ILLEGAL_MOVE_USAGE,
                               parser.get_current_token());
    }
    parser.advance();
    return function_expr::parse(parser, true, false);
}

auto parse_naked_function_expr(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    if (!parser.peek_token_is(syntax::token_type_t::FUNCTION)) {
        return make_syntax_err("'naked' may only appear directly before 'fn'",
                               syntax::error::ILLEGAL_MOVE_USAGE,
                               parser.get_current_token());
    }
    parser.advance();
    return function_expr::parse(parser, false, true);
}

auto function_expr::parse(syntax::parser& parser, bool is_move, bool is_naked)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    TRY(parser.expect_peek(syntax::token_type_t::LPAREN));

    // Parse the definition now that we're at the fn token
    stdx::option<self_parameter> self;
    std::vector<parameter>       parameters;
    bool                         variadic{false};
    if (parser.peek_token_is(syntax::token_type_t::RPAREN)) {
        parser.advance();
    } else if (TRY(try_parse_variadic_fn(parser))) {
        variadic = true;
        TRY(parser.expect_peek(syntax::token_type_t::RPAREN));
    } else {
        // The 'self' parameter can be a value type, ref, or mutable ref
        parser.advance();
        const auto modifier_start{parser.get_current_token()};
        const auto self_modifier{TRY(type_modifier::parse(parser, modifier_start))};
        if (self_modifier.is_volatile()) {
            return make_syntax_err(
                "Self parameters cannot be marked volatile; they must be values, refs, or pointers",
                syntax::error::ILLEGAL_SELF_PARAMETER_MODIFIER,
                modifier_start);
        }

        if (self_modifier.is_value() && (parser.peek_token_is(syntax::token_type_t::COMMA) ||
                                         parser.peek_token_is(syntax::token_type_t::RPAREN))) {
            const identifier_handle ident{TRY(identifier_expr::parse(parser))};
            self.emplace(self_modifier, ident);

            // Still end on a comma
            if (!parser.peek_token_is(syntax::token_type_t::RPAREN)) {
                TRY(parser.expect_peek(syntax::token_type_t::COMMA));
            }
        } else if (!self_modifier.is_value() && parser.peek_token_is(syntax::token_type_t::IDENT)) {
            // Move up to the ident before parsing it
            parser.advance();
            const identifier_handle ident{TRY(identifier_expr::parse(parser))};
            self.emplace(self_modifier, ident);

            // Move to the comma if present
            if (!parser.peek_token_is(syntax::token_type_t::RPAREN)) {
                TRY(parser.expect_peek(syntax::token_type_t::COMMA));
            }
        }

        // The loop starts either on an LPAREN or COMMA
        bool first{true};
        while (!parser.peek_token_is(syntax::token_type_t::RPAREN) &&
               !parser.peek_token_is(syntax::token_type_t::END)) {
            if (TRY(try_parse_variadic_fn(parser))) {
                variadic = true;
                break;
            }

            // If there was no self parameter then we can't advance on the first pass
            if (!first || self) { parser.advance(); }
            const identifier_handle name{TRY(identifier_expr::parse(parser))};
            const auto [param_type, initialized]{TRY(explicit_type::parse_opt_init(parser))};

            // There are no default values for parameters, and they must be explicitly typed
            if (initialized || !param_type) {
                return make_syntax_err("Function parameters may not have default values",
                                       syntax::error::FN_PARAMETER_HAS_DEFAULT_VALUE,
                                       parser.get_location_of(*param_type));
            }

            const auto explicit_type{*param_type};
            if (explicit_type.is<identifier_expr>()) {
                // noreturn is not allowed for parameters
                if (explicit_type.get_token_type() == syntax::token_type_t::NORETURN) {
                    return make_syntax_err("Function parameter types may not be marked `noreturn`",
                                           syntax::error::FN_PARAMETER_IS_NORETURN,
                                           parser.get_location_of(explicit_type));
                }
            }

            parameters.emplace_back(name, explicit_type);
            if (!parser.peek_token_is(syntax::token_type_t::RPAREN)) {
                TRY(parser.expect_peek(syntax::token_type_t::COMMA));
            }
            first = false;
        }
        TRY(parser.expect_peek(syntax::token_type_t::RPAREN));
    }

    TRY(parser.expect_peek(syntax::token_type_t::COLON));
    const auto return_type{TRY(explicit_type::parse(parser))};

    // Function types are decoupled so a block is required here
    if (!parser.peek_token_is(syntax::token_type_t::LBRACE)) {
        return make_syntax_err("Function declarations must have a body",
                               syntax::error::FN_DECLARATION_WITHOUT_BODY,
                               start_token);
    }

    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));
    const block_handle body{TRY(block_stmt::parse(parser))};
    return parser.add_expr<function_expr>(
        start_token, self, std::move(parameters), return_type, body, variadic, is_move, is_naked);
}

auto grouped_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    parser.advance();
    const auto inner{TRY(parser.parse_expression())};
    TRY(parser.expect_peek(syntax::token_type_t::RPAREN));
    parser.mark_parenthesized(*inner);
    return inner;
}

auto identifier_expr::parse(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    if (!start_token.is_valid_ident()) {
        return make_syntax_err(syntax::error::ILLEGAL_IDENTIFIER, start_token);
    }

    return parser.add_expr<identifier_expr>(start_token, start_token.slice);
}

auto if_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};

    bool constexpr_condition{false};
    if (parser.peek_token_is(syntax::token_type_t::CONSTEXPR)) {
        constexpr_condition = true;
        parser.advance();
    }

    // Conditions have to be surrounded by parentheses
    TRY(parser.expect_peek(syntax::token_type_t::LPAREN));
    parser.advance();
    if (parser.current_token_is(syntax::token_type_t::RPAREN)) {
        return make_syntax_err("If expressions must have a condition",
                               syntax::error::IF_MISSING_CONDITION,
                               start_token);
    }

    const auto condition{TRY(parser.parse_expression())};
    TRY(parser.expect_peek(syntax::token_type_t::RPAREN));

    // The consequence and alternate are trivially handled by restricted statement parsers
    parser.advance();
    const auto consequence{TRY(parser.parse_restricted_statement(
        syntax::error::ILLEGAL_IF_BRANCH, syntax::semicolon_behavior::ALLOWED))};
    const auto alternate{TRY(parser.try_parse_restricted_alternate(
        syntax::error::ILLEGAL_IF_BRANCH, syntax::semicolon_behavior::ALLOWED))};

    return parser.add_expr<if_expr>(
        start_token, constexpr_condition, condition, consequence, alternate);
}

auto index_expr::parse(syntax::parser& parser, expr_handle array)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    if (parser.peek_token_is(syntax::token_type_t::RBRACKET)) {
        return make_syntax_err("Cannot index into an array without an index expression",
                               syntax::error::INDEX_MISSING_EXPRESSION,
                               start_token);
    }
    parser.advance();

    const auto idx_expr{TRY(parser.parse_expression())};
    TRY(parser.expect_peek(syntax::token_type_t::RBRACKET));
    return parser.add_expr<index_expr>(start_token, array, idx_expr);
}

auto infinite_loop_expr::parse(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));

    const block_handle block{TRY(block_stmt::parse(parser))};
    return parser.add_expr<infinite_loop_expr>(start_token, block);
}

namespace {

template <NodeData Expr>
[[nodiscard]] auto parse_infix(syntax::parser& parser, expr_handle lhs)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    const auto op_token{parser.get_current_token()};
    if (parser.peek_token_is(syntax::token_type_t::END)) {
        return make_syntax_err("Infix expressions require a right-hand operand",
                               syntax::error::INFIX_MISSING_RHS,
                               op_token);
    }

    auto [current_precedence, current_binding]{parser.get_current_precedence()};
    if (current_binding && current_binding->right_assoc) {
        current_precedence =
            static_cast<syntax::bind_precedence>(std::to_underlying(current_precedence) - 1);
    }

    const auto lhs_start{parser.get_location_of(*lhs)};
    parser.advance();
    const auto rhs{TRY(parser.parse_expression(current_precedence))};
    return parser.add_expr<Expr>(lhs_start, op_token, lhs, rhs);
}

} // namespace

#define MAKE_INFIX_PARSER(Type)                               \
    auto Type::parse(syntax::parser& parser, expr_handle lhs) \
        -> stdx::result<expr_handle, syntax::diagnostic> {    \
        PROFILE_FUNCTION();                                   \
        return parse_infix<Type>(parser, lhs);                \
    }

MAKE_INFIX_PARSER(assignment_expr)
MAKE_INFIX_PARSER(binary_expr)

auto dot_expr::parse(syntax::parser& parser, expr_handle outer)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    TRY(parser.expect_peek(syntax::token_type_t::IDENT));
    const identifier_handle inner{TRY(identifier_expr::parse(parser))};
    return parser.add_expr<dot_expr>(start_token, outer, inner);
}

MAKE_INFIX_PARSER(range_expr)

auto initializer_expr::parse(syntax::parser& parser, stdx::option<expr_handle> object)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};

    std::vector<initializer> initializers;
    while (!parser.peek_token_is(syntax::token_type_t::RBRACE) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        TRY(parser.expect_peek(syntax::token_type_t::DOT));
        implicit_access_handle member{TRY(implicit_access_expr::parse(parser))};

        TRY(parser.expect_peek(syntax::token_type_t::ASSIGN));
        parser.advance();
        const auto value{TRY(parser.parse_expression())};
        initializers.emplace_back(member, value);

        if (!parser.peek_token_is(syntax::token_type_t::RBRACE)) {
            TRY(parser.expect_peek(syntax::token_type_t::COMMA));
        }
    }
    TRY(parser.expect_peek(syntax::token_type_t::RBRACE));

    return parser.add_expr<initializer_expr>(start_token, object, std::move(initializers));
}

auto label_expr::parse(syntax::parser& parser, expr_handle name)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    if (!name.is<identifier_expr>()) {
        return make_syntax_err(
            "Labels may only be identifiers", syntax::error::ILLEGAL_LABEL, start_token);
    }
    parser.advance();

    // The body has to be constructed in place from a lambda as it cannot be default initialized
    const auto raw_stmt{TRY(parser.parse_statement())};
    const auto body{TRY(deconstruct_body(parser, raw_stmt))};

    return parser.add_expr<label_expr>(start_token, name, body);
}

auto label_expr::deconstruct_body(syntax::parser& parser, stmt_handle raw_stmt)
    -> stdx::result<labeled_node_handle, syntax::diagnostic> {
    switch (raw_stmt->get_kind()) {
    case node_kind::EXPRESSION_STATEMENT: {
        const auto& stmt{parser.get_node<expr_stmt>(*raw_stmt)};
        switch (stmt.expression->get_kind()) {
        case node_kind::DO_WHILE_LOOP_EXPRESSION:
        case node_kind::FOR_LOOP_EXPRESSION:
        case node_kind::IF_EXPRESSION:
        case node_kind::INFINITE_LOOP_EXPRESSION:
        case node_kind::MATCH_EXPRESSION:
        case node_kind::WHILE_LOOP_EXPRESSION:    return stmt.expression;
        default:
            return make_syntax_err("Labeled expressions may only be conditionals or loops",
                                   syntax::error::ILLEGAL_LABEL_EXPRESSION,
                                   parser.get_location_of(*raw_stmt));
        }
    }
    case node_kind::BLOCK_STATEMENT: return raw_stmt;
    default:
        return make_syntax_err("Labeled statements may only be blocks",
                               syntax::error::ILLEGAL_LABEL_STATEMENT,
                               parser.get_location_of(*raw_stmt));
    }
}

auto match_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};

    // Conditions have to be surrounded by parentheses
    TRY(parser.expect_peek(syntax::token_type_t::LPAREN));
    parser.advance();
    if (parser.current_token_is(syntax::token_type_t::RPAREN)) {
        return make_syntax_err("Match expressions must have a condition",
                               syntax::error::MATCH_EXPR_MISSING_CONDITION,
                               start_token);
    }

    const auto matcher{TRY(parser.parse_expression())};
    TRY(parser.expect_peek(syntax::token_type_t::RPAREN));

    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));
    if (parser.peek_token_is(syntax::token_type_t::RBRACE)) {
        parser.advance();
        return make_syntax_err("Match expressions must have at least one arm",
                               syntax::error::ARMLESS_MATCH_EXPR,
                               start_token);
    }

    std::vector<arm> arms;
    stdx::opt_size   catch_all_idx;
    usize            arm_idx{0};

    // Current token is either the LBRACE at the start or a comma before parsing
    while (!parser.peek_token_is(syntax::token_type_t::RBRACE) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        parser.advance();

        stdx::option<match_pattern_handle> pattern_opt;
        if (parser.current_token_is(syntax::token_type_t::UNDERSCORE)) {
            if (catch_all_idx) {
                return make_syntax_err("Duplicate catch-all match arm",
                                       syntax::error::ILLEGAL_MATCH_CATCH_ALL,
                                       parser.get_current_token());
            }

            pattern_opt.emplace(parser.add_node<discardable_ident_handle, ast::discarded>(
                parser.get_current_token()));
            catch_all_idx.emplace(arm_idx);
        } else {
            const auto pattern_tok{parser.get_current_token()};
            const auto pattern_raw{TRY(parser.parse_expression())};
            if (!match_pattern_handle::any_compatible(pattern_raw->get_kind())) {
                return make_syntax_err(
                    fmt::format("Unmatchable expression '{}' used as a match arm pattern",
                                pattern_raw->display_name()),
                    syntax::error::ILLEGAL_MATCH_PATTERN,
                    pattern_tok);
            }
            pattern_opt.emplace(pattern_raw);
        }

        const match_pattern_handle pattern{*pattern_opt};
        TRY(parser.expect_peek(syntax::token_type_t::FAT_ARROW));

        // There is an optional capture for every arm
        stdx::option<discardable_ident_handle> capture;
        type_modifier                          modifier;
        if (parser.peek_token_is(syntax::token_type_t::BW_OR)) {
            parser.advance();

            // An underscore is equivalent to a lack of capture; no modifier is allowed on it
            if (parser.peek_token_is(syntax::token_type_t::UNDERSCORE)) {
                parser.advance();
                capture.emplace(parser.add_node<discardable_ident_handle, ast::discarded>(
                    parser.get_current_token()));
            } else {
                // Always check for a modifier and advance past it if present
                parser.advance();
                modifier = type_modifier{parser.get_current_token()};
                if (!modifier.is_value()) { parser.advance(); }

                capture.emplace(TRY(identifier_expr::parse(parser)));
            }
            TRY(parser.expect_peek(syntax::token_type_t::BW_OR));
        }

        if (catch_all_idx == arm_idx && capture) {
            return make_syntax_err("Catch-all match arms may not have a capture clause",
                                   syntax::error::ILLEGAL_MATCH_CATCH_ALL,
                                   parser.get_location_of(*capture));
        }

        // The resulting statement must be restricted like an if branch
        parser.advance();
        const auto consequence{TRY(parser.parse_restricted_statement(
            syntax::error::ILLEGAL_MATCH_ARM, syntax::semicolon_behavior::DISALLOW))};
        arms.emplace_back(pattern, capture, modifier, consequence);
        arm_idx += 1;

        // The lack of a comma must mean we're at the end of the arm list
        if (parser.peek_token_is(syntax::token_type_t::COMMA)) {
            parser.advance();
        } else {
            break;
        }
    }

    TRY(parser.expect_peek(syntax::token_type_t::RBRACE));
    return parser.add_expr<match_expr>(start_token, matcher, std::move(arms), catch_all_idx);
}

namespace {

template <NodeData Expr>
[[nodiscard]] auto parse_prefix(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    const auto prefix_token{parser.get_current_token()};
    if (parser.peek_token_is(syntax::token_type_t::END)) {
        return make_syntax_err("Prefix expressions require an operand",
                               syntax::error::PREFIX_MISSING_OPERAND,
                               prefix_token);
    }
    parser.advance();

    const auto operand{TRY(parser.parse_expression(syntax::bind_precedence::PREFIX))};
    return parser.add_expr<Expr>(prefix_token, operand);
}

} // namespace

#define MAKE_PREFIX_PARSER(Type)                                                                \
    auto Type::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> { \
        PROFILE_FUNCTION();                                                                     \
        return parse_prefix<Type>(parser);                                                      \
    }

MAKE_PREFIX_PARSER(unary_expr)
MAKE_PREFIX_PARSER(reference_expr)
MAKE_PREFIX_PARSER(dereference_expr)
MAKE_PREFIX_PARSER(address_of_expr)

auto implicit_access_expr::parse(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();

    // We need to explicitly jump into the initializer expression here
    if (parser.peek_token_is(syntax::token_type_t::LBRACE)) {
        parser.advance();
        return initializer_expr::parse(parser, stdx::none);
    }

    // Otherwise it suffices to fall back to standard prefix parsing
    const auto prefix_token{parser.get_current_token()};
    if (parser.peek_token_is(syntax::token_type_t::END)) {
        return make_syntax_err("Prefix expressions require an operand",
                               syntax::error::PREFIX_MISSING_OPERAND,
                               prefix_token);
    }

    parser.advance();
    // A trailing `(...)` for implicit calls are picked up by the enclosing Pratt loop.
    const identifier_handle operand{TRY(identifier_expr::parse(parser))};
    return parser.add_expr<implicit_access_expr>(prefix_token, operand);
}

auto string_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    return parser.add_expr<string_expr>(
        start_token, start_token.materialize_string(), start_token.slice);
}

auto module_access_expr::parse(syntax::parser& parser, expr_handle outer)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    if (!outer.any<identifier_expr, module_access_expr, dot_expr>()) {
        return make_syntax_err("Module access expressions must have outer accessors or identifiers",
                               syntax::error::ILLEGAL_OUTER_ACCESSOR_TYPE,
                               parser.get_location_of(*outer));
    }

    const auto start_token{parser.get_current_token()};
    TRY(parser.expect_peek(syntax::token_type_t::IDENT));
    const identifier_handle inner{TRY(identifier_expr::parse(parser))};
    return parser.add_expr<module_access_expr>(start_token, outer, inner);
}

namespace {

[[nodiscard]] auto try_parse_alignment(syntax::parser& parser)
    -> stdx::result<stdx::option<expr_handle>, syntax::diagnostic> {
    stdx::option<expr_handle> explicit_alignment;
    if (parser.peek_token_is(syntax::token_type_t::BUILTIN_ALIGNAS)) {
        parser.advance();
        TRY(parser.expect_peek(syntax::token_type_t::LPAREN));
        parser.advance();
        explicit_alignment.emplace(TRY(parser.parse_expression()));
        TRY(parser.expect_peek(syntax::token_type_t::RPAREN));
    }
    return explicit_alignment;
}

} // namespace

auto struct_expr::parse(syntax::parser& parser, bool is_extern, bool is_packed)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));
    std::vector<field> fields;
    while (!parser.peek_token_is(syntax::token_type_t::RBRACE) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        bool is_public{false};
        auto explicit_alignment{TRY(try_parse_alignment(parser))};

        if (parser.peek_token_is(syntax::token_type_t::PUBLIC)) {
            // Use a transaction to preserve the public modifier
            syntax::parser::transaction transaction{parser};
            parser.advance();

            // With two modifiers a decl is required
            if (parser.get_peek_token().is_member_token()) { break; }

            // There must an ident here since pub is the only modifier
            is_public = true;
            transaction.commit();
            TRY(parser.expect_peek(syntax::token_type_t::IDENT));

        } else if (parser.get_peek_token().is_member_token()) {
            break;
        } else {
            TRY(parser.expect_peek(syntax::token_type_t::IDENT));
        }

        identifier_handle ident{TRY(identifier_expr::parse(parser))};
        TRY(parser.expect_peek(syntax::token_type_t::COLON));
        if (!explicit_alignment) { explicit_alignment = TRY(try_parse_alignment(parser)); }
        const auto type{TRY(explicit_type::parse(parser))};
        if (!explicit_alignment) { explicit_alignment = TRY(try_parse_alignment(parser)); }

        stdx::option<expr_handle> value;
        if (parser.peek_token_is(syntax::token_type_t::ASSIGN)) {
            parser.advance(2);
            value.emplace(TRY(parser.parse_expression()));
        }

        // The identifier holds public information to save space in the field
        if (is_public) { ident->set_token_type(syntax::token_type_t::PUBLIC); }
        fields.emplace_back(ident, type, value, explicit_alignment);

        // No comma means that its the end or that there is a decl list starting
        if (!parser.peek_token_is(syntax::token_type_t::COMMA)) { break; }
        parser.advance();
    }

    auto members{TRY(parse_members(parser))};
    TRY(parser.expect_peek(syntax::token_type_t::RBRACE));
    return parser.add_expr<struct_expr>(
        start_token, std::move(fields), std::move(members), is_extern, is_packed);
}

auto union_expr::parse(syntax::parser& parser, bool is_extern)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));
    std::vector<field> fields;
    while (!parser.peek_token_is(syntax::token_type_t::RBRACE) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        if (parser.get_peek_token().is_member_token()) { break; }

        auto explicit_alignment{TRY(try_parse_alignment(parser))};
        TRY(parser.expect_peek(syntax::token_type_t::IDENT));
        const identifier_handle ident{TRY(identifier_expr::parse(parser))};

        TRY(parser.expect_peek(syntax::token_type_t::COLON));
        if (!explicit_alignment) { explicit_alignment = TRY(try_parse_alignment(parser)); }
        const auto type{TRY(explicit_type::parse(parser))};
        if (!explicit_alignment) { explicit_alignment = TRY(try_parse_alignment(parser)); }

        fields.emplace_back(ident, type, explicit_alignment);

        // No comma means that its the end or that there is a decl list starting
        if (!parser.peek_token_is(syntax::token_type_t::COMMA)) { break; }
        parser.advance();
    }

    auto members{TRY(parse_members(parser))};
    TRY(parser.expect_peek(syntax::token_type_t::RBRACE));

    // Validate here so that there aren't 3 errors spawning from an empty union with decls
    if (fields.empty()) {
        return make_syntax_err("Unions must be declared with at least one field",
                               syntax::error::EMPTY_UNION,
                               start_token);
    }
    return parser.add_expr<union_expr>(
        start_token, std::move(fields), std::move(members), is_extern);
}

auto parse_modified_struct_or_union(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    const auto start_token{parser.get_current_token()};
    bool       is_extern{false};
    bool       is_packed{false};

    while (true) {
        const auto current_tt{parser.get_current_token().type};
        if (current_tt == syntax::token_type_t::EXTERN) {
            is_extern = true;
        } else if (current_tt == syntax::token_type_t::PACKED) {
            is_packed = true;
        } else {
            break;
        }

        if (parser.peek_token_is(syntax::token_type_t::EXTERN) ||
            parser.peek_token_is(syntax::token_type_t::PACKED)) {
            parser.advance();
        } else {
            break;
        }
    }

    if (parser.peek_token_is(syntax::token_type_t::STRUCT)) {
        parser.advance();
        return struct_expr::parse(parser, is_extern, is_packed);
    }

    if (parser.peek_token_is(syntax::token_type_t::UNION)) {
        parser.advance();
        if (is_packed) {
            return make_syntax_err("Unions cannot be declared with `packed`",
                                   syntax::error::ILLEGAL_EXPLICIT_TYPE,
                                   start_token);
        }
        return union_expr::parse(parser, is_extern);
    }

    return make_syntax_err("Expected `struct` or `union` after declaration modifiers",
                           syntax::error::ILLEGAL_EXPLICIT_TYPE,
                           start_token);
}

auto while_loop_expr::parse(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};

    // Conditions have to be surrounded by parentheses
    TRY(parser.expect_peek(syntax::token_type_t::LPAREN));
    parser.advance();
    if (parser.current_token_is(syntax::token_type_t::RPAREN)) {
        return make_syntax_err("While loops must have a corresponding condition",
                               syntax::error::WHILE_MISSING_CONDITION,
                               start_token);
    }

    const auto condition{TRY(parser.parse_expression())};
    TRY(parser.expect_peek(syntax::token_type_t::RPAREN));

    // Continuation expression is optional and is handled as in zig
    stdx::option<expr_handle> continuation;
    if (parser.peek_token_is(syntax::token_type_t::COLON)) {
        const auto continuation_start{parser.get_current_token()};
        parser.advance();
        TRY(parser.expect_peek(syntax::token_type_t::LPAREN));

        // Consume again to look at the actual expr start
        parser.advance();
        if (parser.current_token_is(syntax::token_type_t::RPAREN)) {
            return make_syntax_err("Continuation expression was expected but not found",
                                   syntax::error::EMPTY_WHILE_CONTINUATION,
                                   continuation_start);
        }

        continuation.emplace(TRY(parser.parse_expression()));
        TRY(parser.expect_peek(syntax::token_type_t::RPAREN));
    }

    // Loops must have a well formed block and may have an alternate in non-break cases
    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));
    const block_handle block{TRY(block_stmt::parse(parser))};
    const auto         non_break{
        TRY(parser.try_parse_restricted_alternate(syntax::error::ILLEGAL_LOOP_NON_BREAK))};
    return parser.add_expr<while_loop_expr>(start_token, condition, continuation, block, non_break);
}

} // namespace ghoti::ast
