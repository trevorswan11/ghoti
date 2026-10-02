#include "compiler/ast/expression.hh"

#include <algorithm>
#include <concepts>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/ast.hh"
#include "compiler/ast/attributes.hh"
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
#include "support/counter.hh"

namespace ghoti::ast {

auto array_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    auto       null_terminated{false};

    stdx::option<expr_handle> size;
    auto                      slice_shape{false};
    if (parser.peek_token_is(syntax::token_type_t::RBRACKET)) {
        // `[]T` with nothing between the brackets: a slice type, never a literal
        slice_shape = true;
    } else if (parser.peek_token_is(syntax::token_type_t::NULL_TERMINATED)) {
        // `[:0]T`: a null-terminated slice type
        parser.advance();
        slice_shape     = true;
        null_terminated = true;
    } else {
        parser.advance();
        if (!parser.current_token_is(syntax::token_type_t::UNDERSCORE)) {
            size.emplace(TRY(parser.parse_expression()));
        }

        // The null terminated marker comes after the size for explicitly sized types
        if (parser.peek_token_is(syntax::token_type_t::NULL_TERMINATED)) {
            parser.advance();
            null_terminated = true;
        }
    }

    TRY(parser.expect_peek(syntax::token_type_t::RBRACKET));

    auto mut_elements{false};
    auto poly_elements{false};
    if (parser.peek_token_is(syntax::token_type_t::MUT)) {
        parser.advance();
        mut_elements = true;
        // `[]mut? T`
        if (parser.peek_token_is(syntax::token_type_t::QUESTION)) {
            parser.advance();
            mut_elements  = false;
            poly_elements = true;
        }
    }

    const auto item_type{TRY(explicit_type::parse(parser, true))};

    const auto has_initializer{parser.peek_token_is(syntax::token_type_t::LBRACE)};
    if (slice_shape && has_initializer) {
        // `[]T{ ... }` is not a valid slice literal
        return make_syntax_err(
            "Array literals must be initialized with an implicit or explicit size",
            syntax::error::MISSING_ARRAY_SIZE_TOKEN,
            start_token);
    }

    // No initializer brace: this expression denotes a slice/array *type* value.
    if (!has_initializer) {
        return parser.add_expr<array_expr>(start_token,
                                           size,
                                           null_terminated,
                                           mut_elements,
                                           true,
                                           false,
                                           item_type,
                                           std::vector<expr_handle>{},
                                           poly_elements);
    }

    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));

    // Current token is either the LBRACE at the start or a comma before parsing
    std::vector<expr_handle> items;
    bool                     force_break{false};
    while (!parser.peek_token_is(syntax::token_type_t::RBRACE) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        parser.advance();
        items.emplace_back(TRY(parser.parse_expression()));
        if (!parser.peek_token_is(syntax::token_type_t::RBRACE)) {
            TRY(parser.expect_peek(syntax::token_type_t::COMMA));
            // A comma right before `}` is a trailing comma: keep one item per line.
            force_break = parser.peek_token_is(syntax::token_type_t::RBRACE);
        }
    }

    TRY(parser.expect_peek(syntax::token_type_t::RBRACE));
    return parser.add_expr<array_expr>(start_token,
                                       size,
                                       null_terminated,
                                       mut_elements,
                                       false,
                                       force_break,
                                       item_type,
                                       std::move(items),
                                       poly_elements);
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
[[nodiscard]] auto parse_asm_paren_list(syntax::parser& parser, Fn&& element, bool& force_break)
    -> stdx::result<void, syntax::diagnostic> {
    TRY(parser.expect_peek(syntax::token_type_t::LPAREN));
    while (!parser.peek_token_is(syntax::token_type_t::RPAREN) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        TRY(element());
        if (!parser.peek_token_is(syntax::token_type_t::RPAREN)) {
            TRY(parser.expect_peek(syntax::token_type_t::COMMA));
            // A comma right before `)` is a trailing comma: keep one operand per line.
            if (parser.peek_token_is(syntax::token_type_t::RPAREN)) { force_break = true; }
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
    bool                        force_break{false};

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
            TRY(parse_asm_paren_list(
                parser,
                [&] -> stdx::result<void, syntax::diagnostic> {
                    outputs.emplace_back(TRY(parse_asm_operand(parser)));
                    return {};
                },
                force_break));
        } else if (key == "inputs") {
            TRY(reject_duplicate(seen_inputs, key_token));
            TRY(parse_asm_paren_list(
                parser,
                [&] -> stdx::result<void, syntax::diagnostic> {
                    inputs.emplace_back(TRY(parse_asm_operand(parser)));
                    return {};
                },
                force_break));
        } else if (key == "clobbers") {
            TRY(reject_duplicate(seen_clobbers, key_token));
            TRY(parse_asm_paren_list(
                parser,
                [&] -> stdx::result<void, syntax::diagnostic> {
                    if (!parser.peek_token_is(syntax::token_type_t::STRING)) {
                        return make_syntax_err("An asm clobber must be a string literal",
                                               syntax::error::ASM_MALFORMED_OPERAND,
                                               parser.get_peek_token());
                    }
                    parser.advance();
                    clobbers.emplace_back(string_handle{TRY(string_expr::parse(parser))});
                    return {};
                },
                force_break));
        } else if (key == "options") {
            TRY(reject_duplicate(seen_options, key_token));
            TRY(parse_asm_paren_list(
                parser,
                [&] -> stdx::result<void, syntax::diagnostic> {
                    // Some (`volatile`, `noreturn`) are reserved keywords
                    parser.advance();
                    const auto opt_token{parser.get_current_token()};
                    const auto opt{map_asm_option(opt_token.slice)};
                    if (!opt) {
                        return make_syntax_err(
                            fmt::format("Unknown asm option '{}'", opt_token.slice),
                            syntax::error::ASM_UNKNOWN_OPTION,
                            opt_token);
                    }
                    options.emplace_back(*opt);
                    return {};
                },
                force_break));
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
                                     std::move(options),
                                     force_break);
}

auto call_expr::parse(syntax::parser& parser, expr_handle function)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto            start_token{parser.get_current_token()};
    std::vector<argument> arguments;
    std::vector<bool>     pack_expansions;
    const auto            type_only_scope{[&] -> stdx::option<counter<u32>::guard> {
        switch (function->get_token_type()) {
        case syntax::token_type_t::BUILTIN_TYPE_OF:
        case syntax::token_type_t::BUILTIN_SIZE_OF:
        case syntax::token_type_t::BUILTIN_ALIGN_OF:
        case syntax::token_type_t::BUILTIN_BIT_SIZE_OF: return parser.enter_type_only_operand();
        default:                                        return stdx::none;
        }
    }()};

    // Guaranteed to roll back if there is an error, which it hands back for reporting
    const auto try_parse_expr_argument = [&] -> stdx::option<syntax::diagnostic> {
        // Try an expression first to prevent ambiguity between reference operators
        syntax::parser::transaction transaction{parser};
        parser.advance();
        auto expr{parser.parse_expression()};
        if (!expr) { return std::move(expr.error()); }
        transaction.commit();
        arguments.emplace_back(*expr);
        pack_expansions.emplace_back(false);
        return stdx::none;
    };

    bool force_break{false};
    while (!parser.peek_token_is(syntax::token_type_t::RPAREN) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        if (parser.peek_token_is(syntax::token_type_t::COMMA)) {
            return make_syntax_err("A comma implies an argument but none were found",
                                   syntax::error::COMMA_WITH_MISSING_CALL_ARGUMENT,
                                   parser.get_peek_token());
        }

        // Advance cannot be called here since explicit type relies on peek, not current
        if (auto expr_error{try_parse_expr_argument()}) {
            // Neither parse fits: the expression's error pinpoints the mistake far more often
            auto type_arg{explicit_type::parse(parser)};
            if (!type_arg) { return stdx::err{std::move(*expr_error)}; }
            arguments.emplace_back(*type_arg);
            pack_expansions.emplace_back(false);
        } else if (parser.peek_token_is(syntax::token_type_t::ELLIPSIS)) {
            // `f(pre, rest..., post)`: the argument just parsed is a pack expansion.
            parser.advance();
            pack_expansions.back() = true;
        }
        if (!parser.peek_token_is(syntax::token_type_t::RPAREN)) {
            TRY(parser.expect_peek(syntax::token_type_t::COMMA));
            // A comma right before `)` is a trailing comma: keep one argument per line.
            force_break = parser.peek_token_is(syntax::token_type_t::RPAREN);
        }
    }
    TRY(parser.expect_peek(syntax::token_type_t::RPAREN));

    return parser.add_expr<call_expr>(
        start_token, function, std::move(arguments), force_break, std::move(pack_expansions));
}

namespace {

// An expression in a position that `enabled` makes compile-time (a `comptime` header)
auto parse_compile_time_expression(syntax::parser& parser, bool enabled)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    const syntax::parser::compile_time_scope cx_scope{parser, enabled};
    return parser.parse_expression();
}

} // namespace

auto do_while_loop_expr::parse(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));

    const block_handle block{TRY(block_stmt::parse(parser))};
    TRY(parser.expect_peek(syntax::token_type_t::WHILE));

    bool is_comptime{false};
    if (parser.peek_token_is(syntax::token_type_t::COMPTIME)) {
        parser.advance();
        is_comptime = true;
    }

    TRY(parser.expect_peek(syntax::token_type_t::LPAREN));

    if (parser.peek_token_is(syntax::token_type_t::RPAREN)) {
        return make_syntax_err("While loops must have a condition",
                               syntax::error::WHILE_MISSING_CONDITION,
                               parser.get_current_token());
    }
    parser.advance();

    // There's no continuation or non break clause so this is easy :)
    const auto condition{TRY(parse_compile_time_expression(parser, is_comptime))};
    TRY(parser.expect_peek(syntax::token_type_t::RPAREN));
    return parser.add_expr<do_while_loop_expr>(start_token, block, condition, is_comptime);
}

namespace {

[[nodiscard]] auto deconstruct_member(syntax::parser& parser, stmt_handle member)
    -> stdx::result<member_handle, syntax::diagnostic> {
    switch (member->get_kind()) {
    case node_kind::DECL_STATEMENT:
    case node_kind::IMPORT_STATEMENT: return member_handle{member};
    default:
        return make_syntax_err("Members must be declarations or imports",
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

template <typename Item>
[[nodiscard]] auto
parse_aggregate_cfg_group(syntax::parser& parser,
                          usize           position,
                          stdx::result<Item, syntax::diagnostic> (*parse_one)(syntax::parser&))
    -> stdx::result<cfg_item_group<Item>, syntax::diagnostic> {
    using tt    = syntax::token_type_t;
    using group = cfg_item_group<Item>;
    using arm   = typename group::arm;

    // Current token is `)` (just after a predicate) or `else`; peek is `{` or an item.
    const auto parse_body = [&] -> stdx::result<std::vector<Item>, syntax::diagnostic> {
        std::vector<Item> items;
        if (parser.peek_token_is(tt::LBRACE)) {
            parser.advance(); // current == {
            while (!parser.peek_token_is(tt::RBRACE) && !parser.peek_token_is(tt::END)) {
                if (parser.peek_token_is(tt::COMMA)) {
                    parser.advance();
                    continue;
                }
                items.emplace_back(TRY(parse_one(parser)));
            }
            TRY(parser.expect_peek(tt::RBRACE));
        } else {
            items.emplace_back(TRY(parse_one(parser)));
            if (parser.peek_token_is(tt::COMMA)) { parser.advance(); }
        }
        return items;
    };

    std::vector<arm> arms;
    arms.emplace_back(
        arm{stdx::option<expr_handle>{TRY(parser.parse_cfg_predicate())}, TRY(parse_body())});

    while (parser.peek_token_is(tt::ELSE)) {
        parser.advance(); // current == else
        if (parser.peek_token_is(tt::BUILTIN_CFG)) {
            parser.advance(); // current == @cfg
            auto predicate{TRY(parser.parse_cfg_predicate())};
            arms.emplace_back(arm{stdx::option<expr_handle>{predicate}, TRY(parse_body())});
        } else {
            arms.emplace_back(arm{stdx::none, TRY(parse_body())});
            break; // a predicate-less `else` terminates the chain
        }
    }

    return group{position, std::move(arms)};
}

// Parses one aggregate member (peek is its first token) and validates it.
[[nodiscard]] auto parse_one_member(syntax::parser& parser)
    -> stdx::result<member_handle, syntax::diagnostic> {
    const auto doc_floor{parser.get_current_token().line};
    parser.advance();
    auto       parsed{TRY(parser.parse_statement())};
    const auto member{TRY(deconstruct_member(parser, parsed))};
    if (const auto err_msg{validate_member_decl(parser, member)}) {
        return make_syntax_err(
            std::string{*err_msg}, syntax::error::INVALID_MEMBER, parser.get_location_of(*member));
    }

    // Carry a leading `///` block onto a `const` / `let` / `let mut` member the same way top-level
    // decls get theirs, so it is available on hover.
    if (const auto decl{parser.get_ast().get_as_opt<decl_stmt>(*member)}) {
        parser.attach_member_doc(decl->name, doc_floor);
    }
    return member;
}

using member_cfg_group = cfg_item_group<member_handle>;

// Reports whether its first @cfg arm holds members rather than fields/variants
[[nodiscard]] auto cfg_group_targets_members(syntax::parser& parser) -> bool {
    using tt = syntax::token_type_t;
    syntax::parser::transaction tx{parser};
    parser.advance(); // current == @cfg
    if (!parser.peek_token_is(tt::LPAREN)) { return false; }
    parser.advance(); // current == (
    usize depth{1};
    while (depth > 0 && !parser.current_token_is(tt::END)) {
        parser.advance();
        if (parser.current_token_is(tt::LPAREN)) {
            ++depth;
        } else if (parser.current_token_is(tt::RPAREN)) {
            --depth;
        }
    }
    if (parser.peek_token_is(tt::LBRACE)) { parser.advance(); }
    if (parser.peek_token_is(tt::AT_LBRACKET)) {
        parser.advance();
        if (!parse_attribute_list(parser)) { return false; }
    }
    if (parser.peek_token_is(tt::PUBLIC)) { parser.advance(); }
    return parser.get_peek_token().is_member_token();
}

[[nodiscard]] auto parse_members(syntax::parser& parser, std::vector<member_cfg_group>& cfg_groups)
    -> stdx::result<member_list, syntax::diagnostic> {
    member_list members;
    while (!parser.peek_token_is(syntax::token_type_t::RBRACE) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        if (parser.peek_token_is(syntax::token_type_t::BUILTIN_CFG)) {
            parser.advance(); // current == @cfg
            cfg_groups.emplace_back(TRY(parse_aggregate_cfg_group<member_handle>(
                parser, members.size(), parse_one_member)));
            if (parser.peek_token_is(syntax::token_type_t::COMMA)) { parser.advance(); }
            continue;
        }
        members.emplace_back(TRY(parse_one_member(parser)));
    }
    return members;
}

// Whether the `@[...]` at the peek token precedes a member declaration rather than a field
[[nodiscard]] auto attributes_precede_member(syntax::parser& parser) -> bool {
    syntax::parser::transaction tx{parser};
    parser.advance(); // current == @[
    if (!parse_attribute_list(parser)) { return false; }
    if (parser.peek_token_is(syntax::token_type_t::PUBLIC)) { parser.advance(); }
    return parser.get_peek_token().is_member_token();
}

// A field's optional leading `@[...]`; leaves the peek token on the field's first token
[[nodiscard]] auto try_parse_field_attributes(syntax::parser& parser)
    -> stdx::result<stdx::option<attribute_list>, syntax::diagnostic> {
    if (!parser.peek_token_is(syntax::token_type_t::AT_LBRACKET)) { return stdx::none; }
    parser.advance();
    return TRY(parse_attribute_list(parser));
}

[[nodiscard]] auto parse_struct_field(syntax::parser& parser)
    -> stdx::result<struct_expr::field, syntax::diagnostic> {
    using tt = syntax::token_type_t;
    const auto doc_floor{parser.get_current_token().line};
    auto       attributes{TRY(try_parse_field_attributes(parser))};
    bool       is_public{false};
    if (parser.peek_token_is(tt::PUBLIC)) {
        parser.advance();
        is_public = true;
    }

    TRY(parser.expect_peek(tt::IDENT));
    identifier_handle ident{TRY(identifier_expr::parse(parser))};
    parser.attach_member_doc(ident, doc_floor);
    TRY(parser.expect_peek(tt::COLON));
    const auto                type{TRY(explicit_type::parse(parser))};
    stdx::option<expr_handle> value;
    if (parser.peek_token_is(tt::ASSIGN)) {
        parser.advance(2);
        value.emplace(TRY(parser.parse_expression()));
    }
    if (is_public) { ident->set_token_type(tt::PUBLIC); }
    return struct_expr::field{ident, type, value, std::move(attributes)};
}

[[nodiscard]] auto parse_union_field(syntax::parser& parser)
    -> stdx::result<union_expr::field, syntax::diagnostic> {
    using tt = syntax::token_type_t;
    const auto doc_floor{parser.get_current_token().line};
    auto       attributes{TRY(try_parse_field_attributes(parser))};
    TRY(parser.expect_peek(tt::IDENT));
    const identifier_handle ident{TRY(identifier_expr::parse(parser))};
    parser.attach_member_doc(ident, doc_floor);
    TRY(parser.expect_peek(tt::COLON));
    const auto type{TRY(explicit_type::parse(parser))};
    return union_expr::field{ident, type, std::move(attributes)};
}

[[nodiscard]] auto parse_enumeration(syntax::parser& parser)
    -> stdx::result<enum_expr::enumeration, syntax::diagnostic> {
    using tt = syntax::token_type_t;
    const auto doc_floor{parser.get_current_token().line};
    TRY(parser.expect_peek(tt::IDENT));
    const identifier_handle ident{TRY(identifier_expr::parse(parser))};
    parser.attach_member_doc(ident, doc_floor);
    stdx::option<expr_handle> value;
    if (parser.peek_token_is(tt::ASSIGN)) {
        parser.advance(2);
        value.emplace(TRY(parser.parse_expression()));
    }
    return enum_expr::enumeration{ident, value};
}

} // namespace

auto enum_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};

    stdx::option<explicit_type_id> underlying;
    if (parser.peek_token_is(syntax::token_type_t::COLON)) {
        parser.advance();
        underlying.emplace(TRY(explicit_type::parse(parser)));
    }
    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));

    bool                     non_exhaustive{false};
    bool                     force_break{false};
    std::vector<enumeration> enumerations;
    std::vector<cfg_group>   cfg_groups;
    while (!parser.peek_token_is(syntax::token_type_t::RBRACE) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        if (parser.peek_token_is(syntax::token_type_t::BUILTIN_CFG)) {
            if (cfg_group_targets_members(parser)) { break; } // handled by parse_members
            parser.advance();                                 // current == @cfg
            cfg_groups.emplace_back(TRY(parse_aggregate_cfg_group<enumeration>(
                parser, enumerations.size(), parse_enumeration)));
            if (parser.peek_token_is(syntax::token_type_t::COMMA)) { parser.advance(); }
            continue;
        }
        if (parser.get_peek_token().is_member_token()) { break; }

        if (parser.peek_token_is(syntax::token_type_t::UNDERSCORE)) {
            parser.advance();
            bool marker_comma{false};
            if (parser.peek_token_is(syntax::token_type_t::COMMA)) {
                parser.advance();
                marker_comma = true;
            }

            // The non exhaustive marker must be the final 'enumeration'
            if (parser.get_peek_token().is_member_token() ||
                parser.peek_token_is(syntax::token_type_t::RBRACE) ||
                parser.peek_token_is(syntax::token_type_t::BUILTIN_CFG)) {
                non_exhaustive = true;
                force_break |= marker_comma && parser.peek_token_is(syntax::token_type_t::RBRACE);
                break;
            }

            return make_syntax_err(
                "The underscore in non-exhaustive enums must be the last enumeration",
                syntax::error::ILLEGAL_NON_EXHAUSTIVE_ENUM,
                parser.get_current_token());
        }

        const auto doc_floor{parser.get_current_token().line};
        TRY(parser.expect_peek(syntax::token_type_t::IDENT));
        const identifier_handle ident{TRY(identifier_expr::parse(parser))};
        parser.attach_member_doc(ident, doc_floor);

        stdx::option<expr_handle> value;
        if (parser.peek_token_is(syntax::token_type_t::ASSIGN)) {
            parser.advance(2);
            value.emplace(TRY(parser.parse_expression()));
        }
        enumerations.emplace_back(ident, value);

        // No comma means that its the end or that there is a decl list starting
        const bool had_comma{parser.peek_token_is(syntax::token_type_t::COMMA)};
        if (had_comma) { parser.advance(); }
        parser.attach_member_doc(ident, doc_floor); // a trailing `///` is siphoned only now
        if (!had_comma) { break; }
        // A comma right before `}` is a trailing comma: keep one variant per line.
        force_break |= parser.peek_token_is(syntax::token_type_t::RBRACE);
    }

    std::vector<member_cfg_group> member_cfg_groups;
    auto                          members{TRY(parse_members(parser, member_cfg_groups))};
    TRY(parser.expect_peek(syntax::token_type_t::RBRACE));

    // Validate here so that there aren't 3 errors spawning from an empty enum with decls
    if (!non_exhaustive && enumerations.empty() && cfg_groups.empty()) {
        return make_syntax_err("Enums must be declared with at least one enumeration",
                               syntax::error::EMPTY_ENUM,
                               start_token);
    }
    return parser.add_expr<enum_expr>(start_token,
                                      underlying,
                                      std::move(enumerations),
                                      std::move(cfg_groups),
                                      non_exhaustive,
                                      force_break,
                                      std::move(members),
                                      std::move(member_cfg_groups));
}

auto for_loop_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};

    bool is_comptime{false};
    if (parser.peek_token_is(syntax::token_type_t::COMPTIME)) {
        parser.advance();
        is_comptime = true;
    }

    // Iterables have to be surrounded by parentheses
    TRY(parser.expect_peek(syntax::token_type_t::LPAREN));
    if (parser.peek_token_is(syntax::token_type_t::RPAREN)) {
        return make_syntax_err("For loops must contain at least one iterable",
                               syntax::error::FOR_MISSING_ITERABLES,
                               start_token);
    }

    std::vector<expr_handle>                 iterables;
    bool                                     iterables_force_break{false};
    const syntax::parser::compile_time_scope cx_scope{parser, is_comptime};
    while (!parser.peek_token_is(syntax::token_type_t::RPAREN) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        parser.advance();

        const auto iterable{TRY(parser.parse_expression())};
        iterables.emplace_back(iterable);

        if (!parser.peek_token_is(syntax::token_type_t::RPAREN)) {
            TRY(parser.expect_peek(syntax::token_type_t::COMMA));
            iterables_force_break = parser.peek_token_is(syntax::token_type_t::RPAREN);
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
    bool                 captures_force_break{false};
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
            captures_force_break = parser.peek_token_is(syntax::token_type_t::BW_OR);
        }
    }
    TRY(parser.expect_peek(syntax::token_type_t::BW_OR));

    // Loops must have a well formed block and may have an alternate in non-break cases
    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));
    const block_handle        block{TRY(block_stmt::parse(parser))};
    stdx::option<stmt_handle> non_break;
    if (is_comptime) {
        if (parser.peek_token_is(syntax::token_type_t::ELSE)) {
            return make_syntax_err("`for comptime` cannot have an `else`/non-break clause",
                                   syntax::error::COMPTIME_LOOP_HAS_ELSE,
                                   parser.get_peek_token());
        }
    } else {
        non_break =
            TRY(parser.try_parse_restricted_alternate(syntax::error::ILLEGAL_LOOP_NON_BREAK));
    }

    // The number of captures must align with the number of iterables
    if (captures.size() != iterables.size()) {
        return make_syntax_err("The number of for loop captures must match the number of iterables",
                               syntax::error::FOR_ITERABLE_CAPTURE_MISMATCH,
                               start_token);
    }

    return parser.add_expr<for_loop_expr>(start_token,
                                          std::move(iterables),
                                          std::move(captures),
                                          block,
                                          non_break,
                                          iterables_force_break,
                                          captures_force_break,
                                          is_comptime);
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

namespace {

// A `const` function's param read by a compile-time position binds at compile time (#337)
auto infer_comptime_params(syntax::parser&                             parser,
                           const syntax::parser::comptime_param_frame& frame,
                           std::vector<function_expr::parameter>&      parameters) -> void {
    if (!frame.infer) { return; }
    for (auto& param : parameters) {
        if (param.is_pack || !param.name.is<identifier_expr>()) { continue; }
        // A `T: type` param is already compile-time known
        if (param.explicit_type.is_valid() &&
            param.explicit_type.get_token_type() == syntax::token_type_t::TYPE_TYPE) {
            continue;
        }
        const auto name{parser.get_ast().get_as<identifier_expr>(param.name).name};
        if (std::ranges::contains(frame.compile_time_names, name)) { param.is_comptime = true; }
    }
}

} // namespace

auto parse_move_function_expr(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    if (!parser.peek_token_is(syntax::token_type_t::FUNCTION)) {
        return make_syntax_err("'move' may only appear directly before 'fn'",
                               syntax::error::ILLEGAL_MOVE_USAGE,
                               parser.get_current_token());
    }
    parser.advance();
    return function_expr::parse(parser, true);
}

auto parse_attributed_function_expr(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    using tt = syntax::token_type_t;
    auto        attributes{TRY(parse_attribute_list(parser))};
    const auto& next{parser.get_peek_token()};
    if (next.type != tt::FUNCTION && next.type != tt::MOVE) {
        return make_syntax_err("An attribute list in expression position must precede a function "
                               "literal",
                               syntax::error::MISPLACED_ATTRIBUTES,
                               next);
    }
    parser.advance();
    const auto parse_fn{*syntax::parser::get_prefix_fn_opt(parser.get_current_token().type)};
    const auto fn{TRY(parse_fn(parser))};

    auto& fn_node{parser.get_ast().get_as_mut<function_expr>(*fn)};
    if (fn_node.is_type_expr) {
        return make_syntax_err("Attributes apply to function definitions, not function types",
                               syntax::error::MISPLACED_ATTRIBUTES,
                               parser.get_location_of(*fn));
    }
    fn_node.attributes.emplace(std::move(attributes));
    return fn;
}

// Optional `callconv(.ident)` between the parameter list and the return-type colon.
[[nodiscard]] auto try_parse_callconv(syntax::parser& parser)
    -> stdx::result<calling_convention, syntax::diagnostic> {
    if (!parser.peek_token_is(syntax::token_type_t::CALLCONV)) { return calling_convention::C; }
    parser.advance();
    TRY(parser.expect_peek(syntax::token_type_t::LPAREN));
    TRY(parser.expect_peek(syntax::token_type_t::DOT));
    TRY(parser.expect_peek(syntax::token_type_t::IDENT));
    const auto name_token{parser.get_current_token()};
    const auto conv{calling_convention_from_name(name_token.slice)};
    if (!conv) {
        return make_syntax_err(fmt::format("Unknown calling convention '.{}'", name_token.slice),
                               syntax::error::ILLEGAL_DECL_MODIFIERS,
                               name_token);
    }
    TRY(parser.expect_peek(syntax::token_type_t::RPAREN));
    return *conv;
}

auto function_expr::parse(syntax::parser& parser, bool is_move, bool is_extern)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto                           start_token{parser.get_current_token()};
    const syntax::parser::function_scope fn_scope{parser};
    TRY(parser.expect_peek(syntax::token_type_t::LPAREN));

    // Parse the definition now that we're at the fn token
    stdx::option<self_parameter>           self;
    std::vector<parameter>                 parameters;
    std::vector<function_expr::impl_bound> impl_bounds;
    bool                                   variadic{false};
    bool                                   params_force_break{false}; // trailing comma in `fn(...)`
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
            // Skip only for the first param with no self: current already sits on its name
            if (!(first && !self) && TRY(try_parse_variadic_fn(parser))) {
                variadic = true;
                break;
            }

            // If there was no self parameter then we can't advance on the first pass
            if (!first || self) { parser.advance(); }

            bool is_comptime{false};
            if (parser.current_token_is(syntax::token_type_t::COMPTIME)) {
                is_comptime = true;
                parser.advance();
            }

            const auto name{parser.current_token_is(syntax::token_type_t::UNDERSCORE)
                                ? parser.add_node<discardable_ident_handle, ast::discarded>(
                                      parser.get_current_token())
                                : discardable_ident_handle{TRY(identifier_expr::parse(parser))}};

            // An untyped pack (`rest...`) has no `: Type` at all; check before requiring one.
            bool is_pack{false};
            auto param_explicit_type{explicit_type_id::make_invalid()};
            if (parser.peek_token_is(syntax::token_type_t::ELLIPSIS)) {
                parser.advance();
                is_pack = true;
            } else {
                const auto [param_type, initialized]{TRY(explicit_type::parse_opt_init(parser))};

                // There are no default values for parameters, and they must be explicitly typed
                if (!param_type) {
                    return make_syntax_err("Function parameters must be explicitly typed",
                                           syntax::error::FN_PARAMETER_HAS_DEFAULT_VALUE,
                                           parser.get_current_token());
                }
                if (initialized) {
                    return make_syntax_err("Function parameters may not have default values",
                                           syntax::error::FN_PARAMETER_HAS_DEFAULT_VALUE,
                                           parser.get_location_of(*param_type));
                }

                param_explicit_type = *param_type;
                if (param_explicit_type.is<identifier_expr>()) {
                    // noreturn is not allowed for parameters
                    if (param_explicit_type.get_token_type() == syntax::token_type_t::NORETURN) {
                        return make_syntax_err(
                            "Function parameter types may not be marked `noreturn`",
                            syntax::error::FN_PARAMETER_IS_NORETURN,
                            parser.get_location_of(param_explicit_type));
                    }
                }

                if (auto bound{parser.take_pending_impl_bound()}) {
                    impl_bounds.emplace_back(function_expr::impl_bound{
                        .param_index = static_cast<u32>(parameters.size()),
                        .interfaces  = std::move(*bound),
                    });

                    // `rest: impl I...`: the pack marker follows the bound's type expression.
                    if (parser.peek_token_is(syntax::token_type_t::ELLIPSIS)) {
                        parser.advance();
                        is_pack = true;
                    }
                }
            }

            parameters.emplace_back(name, param_explicit_type, is_comptime, is_pack, is_comptime);
            if (!parser.peek_token_is(syntax::token_type_t::RPAREN)) {
                TRY(parser.expect_peek(syntax::token_type_t::COMMA));
                // A comma immediately before `)` is a trailing comma: keep one param per line.
                params_force_break = parser.peek_token_is(syntax::token_type_t::RPAREN);
                if (is_pack && !params_force_break) {
                    return make_syntax_err("A parameter pack must be the last parameter",
                                           syntax::error::PACK_PARAM_NOT_LAST,
                                           parser.get_current_token());
                }
            }
            first = false;
        }
        TRY(parser.expect_peek(syntax::token_type_t::RPAREN));
    }

    const bool has_explicit_conv{parser.peek_token_is(syntax::token_type_t::CALLCONV)};
    const auto conv{TRY(try_parse_callconv(parser))};
    TRY(parser.expect_peek(syntax::token_type_t::COLON));
    // A `fn(...): R` return type leaves the following `{` for this literal's own body
    const auto return_type{TRY(explicit_type::parse(parser, true))};

    if (is_extern && parser.peek_token_is(syntax::token_type_t::LBRACE)) {
        return make_syntax_err("`extern fn(...)` names a function pointer type and cannot have a "
                               "body",
                               syntax::error::EXPLICIT_FN_TYPE_HAS_BODY,
                               start_token);
    }

    // No body: `fn(params): ret` is a function*type value
    if (!parser.peek_token_is(syntax::token_type_t::LBRACE)) {
        for (const auto& param : parameters) {
            if (param.name.is<ast::discarded>()) {
                return make_syntax_err("Function type parameter names cannot be discarded; a "
                                       "parameter name is required",
                                       syntax::error::FN_TYPE_PARAMETER_DISCARDED,
                                       parser.get_location_of(*param.name));
            }
        }
        return parser.add_expr<function_expr>(start_token,
                                              self,
                                              std::move(parameters),
                                              return_type,
                                              block_handle::make_invalid(),
                                              variadic,
                                              is_move,
                                              true,
                                              params_force_break,
                                              conv,
                                              has_explicit_conv,
                                              is_extern,
                                              std::move(impl_bounds));
    }

    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));
    const block_handle body{TRY(block_stmt::parse(parser))};
    infer_comptime_params(parser, fn_scope.frame(), parameters);
    return parser.add_expr<function_expr>(start_token,
                                          self,
                                          std::move(parameters),
                                          return_type,
                                          body,
                                          variadic,
                                          is_move,
                                          false,
                                          params_force_break,
                                          conv,
                                          has_explicit_conv,
                                          false,
                                          std::move(impl_bounds));
}

auto grouped_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    parser.advance();
    const auto inner{TRY(parser.parse_expression())};
    TRY(parser.expect_peek(syntax::token_type_t::RPAREN));
    parser.mark_parenthesized(*inner);
    return inner;
}

auto parse_identifier_reference(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    const auto ident{TRY(identifier_expr::parse(parser))};
    parser.note_identifier_reference(parser.get_ast().get_as<identifier_expr>(ident).name);
    return ident;
}

auto identifier_expr::parse(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    if (!start_token.is_valid_ident()) {
        return make_syntax_err(fmt::format("Expected an identifier, found '{}'", start_token.slice),
                               syntax::error::ILLEGAL_IDENTIFIER,
                               start_token);
    }

    if (start_token.is_raw_identifier()) {
        auto decoded{start_token.materialize_raw_identifier()};
        if (decoded.empty()) {
            return make_syntax_err("A raw identifier cannot be empty",
                                   syntax::error::EMPTY_RAW_IDENTIFIER,
                                   start_token);
        }
        return parser.add_expr<identifier_expr>(start_token, parser.get_ast().intern(decoded));
    }

    return parser.add_expr<identifier_expr>(start_token,
                                            parser.get_ast().intern(start_token.slice));
}

namespace {

// `|v|`, `|&mut v|`, or `|_|` after the current token, or none when no `|` follows
auto try_parse_capture(syntax::parser& parser)
    -> stdx::result<stdx::option<capture>, syntax::diagnostic> {
    if (!parser.peek_token_is(syntax::token_type_t::BW_OR)) { return stdx::none; }
    parser.advance(); // current == |

    type_modifier modifier;
    if (parser.peek_token_is(syntax::token_type_t::UNDERSCORE)) {
        parser.advance();
        const auto discarded{
            parser.add_node<discardable_ident_handle, ast::discarded>(parser.get_current_token())};
        TRY(parser.expect_peek(syntax::token_type_t::BW_OR));
        return capture{.modifier = modifier, .payload = discarded};
    }

    parser.advance();
    modifier = type_modifier{parser.get_current_token()};
    if (!modifier.is_value()) {
        parser.advance();
        if (parser.current_token_is(syntax::token_type_t::UNDERSCORE)) {
            return make_syntax_err("A discarded capture `_` can't take a modifier",
                                   syntax::error::ILLEGAL_CAPTURE,
                                   parser.get_current_token());
        }
    }
    const discardable_ident_handle payload{TRY(identifier_expr::parse(parser))};
    TRY(parser.expect_peek(syntax::token_type_t::BW_OR));
    return capture{.modifier = modifier, .payload = payload};
}

struct capturing_alternate {
    stdx::option<stmt_handle> body;
    stdx::option<capture>     capture;
};

// `else B` / `else |e| B`; an `else` capture needs a payload capture to pair with
auto try_parse_capturing_alternate(syntax::parser& parser,
                                   bool            has_payload_capture,
                                   syntax::error   error)
    -> stdx::result<capturing_alternate, syntax::diagnostic> {
    if (!parser.peek_token_is(syntax::token_type_t::ELSE)) { return capturing_alternate{}; }
    parser.advance(); // current == else
    const auto else_token{parser.get_current_token()};
    auto       else_capture{TRY(try_parse_capture(parser))};
    if (else_capture && !has_payload_capture) {
        return make_syntax_err("An `else` capture needs a payload capture after the condition",
                               syntax::error::ILLEGAL_CAPTURE,
                               else_token);
    }
    parser.advance(); // current == the alternate's first token
    auto body{TRY(parser.parse_restricted_statement(error, syntax::semicolon_behavior::ALLOWED))};
    return capturing_alternate{.body = body, .capture = std::move(else_capture)};
}

} // namespace

auto if_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};

    bool comptime_condition{false};
    if (parser.peek_token_is(syntax::token_type_t::COMPTIME)) {
        comptime_condition = true;
        parser.advance();
    }

    // `if comptime a else b` has no condition: it branches on the evaluation context itself
    stdx::option<expr_handle> condition;
    if (!(comptime_condition && !parser.peek_token_is(syntax::token_type_t::LPAREN))) {
        // Conditions have to be surrounded by parentheses
        TRY(parser.expect_peek(syntax::token_type_t::LPAREN));
        parser.advance();
        if (parser.current_token_is(syntax::token_type_t::RPAREN)) {
            return make_syntax_err("If expressions must have a condition",
                                   syntax::error::IF_MISSING_CONDITION,
                                   start_token);
        }

        const syntax::parser::compile_time_scope cx_scope{parser, comptime_condition};
        condition.emplace(TRY(parser.parse_expression()));
        TRY(parser.expect_peek(syntax::token_type_t::RPAREN));
    }
    auto payload_capture{condition ? TRY(try_parse_capture(parser)) : stdx::none};

    // The consequence and alternate are trivially handled by restricted statement parsers
    parser.advance();
    const auto consequence{TRY(parser.parse_restricted_statement(
        syntax::error::ILLEGAL_IF_BRANCH, syntax::semicolon_behavior::ALLOWED))};
    auto       alternate{TRY(try_parse_capturing_alternate(
        parser, payload_capture.has_value(), syntax::error::ILLEGAL_IF_BRANCH))};

    return parser.add_expr<if_expr>(start_token,
                                    comptime_condition,
                                    condition,
                                    consequence,
                                    alternate.body,
                                    std::move(payload_capture),
                                    std::move(alternate.capture));
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
    bool       is_comptime{false};
    if (parser.peek_token_is(syntax::token_type_t::COMPTIME)) {
        parser.advance();
        is_comptime = true;
    }
    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));

    const block_handle block{TRY(block_stmt::parse(parser))};
    return parser.add_expr<infinite_loop_expr>(start_token, block, is_comptime);
}

auto cfg_value_expr::parse(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    using syntax::token_type_t;
    const auto start_token{parser.get_current_token()};

    TRY(parser.expect_peek(token_type_t::LPAREN));
    parser.advance();
    if (parser.current_token_is(token_type_t::RPAREN)) {
        return make_syntax_err("@cfgValue requires an argument",
                               syntax::error::CFG_VALUE_MISSING_ARGUMENT,
                               start_token);
    }

    std::vector<guard>        guards;
    stdx::option<expr_handle> fallback;
    bool                      force_break{false};

    // Parses one `<pred> => <val>` or `_ => <val>` arm
    const auto parse_guard_arm = [&] -> stdx::result<void, syntax::diagnostic> {
        if (parser.current_token_is(token_type_t::UNDERSCORE)) {
            if (fallback) {
                return make_syntax_err("@cfgValue guard form allows only one '_ =>' arm",
                                       syntax::error::CFG_VALUE_DUPLICATE_FALLBACK,
                                       parser.get_current_token());
            }
            TRY(parser.expect_peek(token_type_t::FAT_ARROW));
            parser.advance();
            fallback.emplace(TRY(parser.parse_expression()));
            return {};
        }
        const auto predicate{TRY(parser.parse_expression())};
        TRY(parser.expect_peek(token_type_t::FAT_ARROW));
        parser.advance();
        const auto value{TRY(parser.parse_expression())};
        guards.emplace_back(guard{predicate, value});
        return {};
    };

    if (!parser.current_token_is(token_type_t::UNDERSCORE)) {
        const auto first{TRY(parser.parse_expression())};
        if (!parser.peek_token_is(token_type_t::FAT_ARROW)) {
            TRY(parser.expect_peek(token_type_t::RPAREN));
            return parser.add_expr<cfg_value_expr>(start_token,
                                                   stdx::option<expr_handle>{first},
                                                   std::vector<guard>{},
                                                   stdx::option<expr_handle>{});
        }
        parser.advance(2); // current = =>, then value's first token
        guards.emplace_back(guard{first, TRY(parser.parse_expression())});
    } else {
        TRY(parse_guard_arm());
    }

    while (parser.peek_token_is(token_type_t::COMMA)) {
        parser.advance(); // ,
        if (parser.peek_token_is(token_type_t::RPAREN)) {
            force_break = true; // trailing comma: keep one arm per line
            break;
        }
        parser.advance(); // arm's first token
        TRY(parse_guard_arm());
    }

    TRY(parser.expect_peek(token_type_t::RPAREN));
    if (guards.empty() && !fallback) {
        return make_syntax_err("@cfgValue guard form requires at least one arm",
                               syntax::error::CFG_VALUE_EMPTY_GUARD,
                               start_token);
    }
    return parser.add_expr<cfg_value_expr>(start_token,
                                           stdx::option<expr_handle>{},
                                           std::move(guards),
                                           std::move(fallback),
                                           force_break);
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

namespace {

// A keyword right after `.` can only be a name, so `.weak` and `x.type` read as members
[[nodiscard]] auto is_keyword_member(const syntax::token_t& token) noexcept -> bool {
    return token.type != syntax::token_type_t::IDENT && token.slice != "_" &&
           syntax::token_type::is_valid_identifier_name(token.slice);
}

// The name after a `.`, with the parser on it
[[nodiscard]] auto parse_member_name(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    auto token{parser.get_current_token()};
    if (!is_keyword_member(token)) { return identifier_expr::parse(parser); }
    const auto name{parser.get_ast().intern(token.slice)};
    token.type = syntax::token_type_t::IDENT;
    return parser.add_expr<identifier_expr>(token, name);
}

} // namespace

auto dot_expr::parse(syntax::parser& parser, expr_handle outer)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    if (is_keyword_member(parser.get_peek_token())) {
        parser.advance();
    } else {
        TRY(parser.expect_peek(syntax::token_type_t::IDENT));
    }
    const identifier_handle inner{TRY(parse_member_name(parser))};
    return parser.add_expr<dot_expr>(start_token, outer, inner);
}

namespace {

[[nodiscard]] auto parse_range_upper(syntax::parser& parser, syntax::bind_precedence prec)
    -> stdx::result<stdx::option<expr_handle>, syntax::diagnostic> {
    if (!syntax::parser::get_prefix_fn_opt(parser.get_peek_token().type)) {
        return stdx::option<expr_handle>{};
    }
    parser.advance();
    return stdx::option<expr_handle>{TRY(parser.parse_expression(prec))};
}

} // namespace

auto range_expr::parse(syntax::parser& parser, expr_handle lhs)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto op_token{parser.get_current_token()};
    const auto lhs_start{parser.get_location_of(*lhs)};
    const auto prec{parser.get_current_precedence().first};
    const auto rhs{TRY(parse_range_upper(parser, prec))};
    return parser.add_expr<range_expr>(lhs_start, op_token, stdx::option<expr_handle>{lhs}, rhs);
}

auto range_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto op_token{parser.get_current_token()};
    const auto prec{parser.get_current_precedence().first};
    const auto rhs{TRY(parse_range_upper(parser, prec))};
    return parser.add_expr<range_expr>(op_token, stdx::option<expr_handle>{}, rhs);
}

auto unwrap_expr::parse(syntax::parser& parser, expr_handle lhs)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto op_token{parser.get_current_token()};
    const auto lhs_start{parser.get_location_of(*lhs)};
    return parser.add_expr<unwrap_expr>(lhs_start, op_token, lhs);
}

auto initializer_expr::parse(syntax::parser& parser, stdx::option<expr_handle> object)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};

    std::vector<initializer> initializers;
    bool                     force_break{false};
    while (!parser.peek_token_is(syntax::token_type_t::RBRACE) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        stdx::option<implicit_access_handle> member;
        stdx::option<expr_handle>            positional_value;
        if (parser.peek_token_is(syntax::token_type_t::DOT)) {
            parser.advance();
            const auto access{TRY(implicit_access_expr::parse(parser))};
            if (access->is<initializer_expr>()) {
                // A bare `.{...}` used positionally, e.g. as an array element: `implicit_access`
                // already jumped into `initializer_expr::parse` and consumed the whole thing.
                positional_value.emplace(access);
            } else {
                // Named entry: `.field = value`
                member.emplace(access);
                TRY(parser.expect_peek(syntax::token_type_t::ASSIGN));
                parser.advance();
            }
        } else {
            // Positional entry: `value` (array-style `T{ a, b, c }`)
            parser.advance();
        }
        const auto value{positional_value ? *positional_value : TRY(parser.parse_expression())};
        initializers.emplace_back(member, value);

        if (!parser.peek_token_is(syntax::token_type_t::RBRACE)) {
            TRY(parser.expect_peek(syntax::token_type_t::COMMA));
            // A comma right before `}` is a trailing comma: keep one entry per line.
            force_break = parser.peek_token_is(syntax::token_type_t::RBRACE);
        }
    }
    TRY(parser.expect_peek(syntax::token_type_t::RBRACE));

    return parser.add_expr<initializer_expr>(
        start_token, object, std::move(initializers), force_break);
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

    return parser.add_expr<label_expr>(
        start_token, stdx::option<identifier_handle>{identifier_handle{name}}, body);
}

auto label_expr::is_comptime(const AST& ast) const noexcept -> bool {
    if (const auto block{ast.get_as_opt<block_stmt>(body)}) { return block->is_comptime; }
    return false;
}

auto parse_comptime_expr(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};

    if (parser.peek_token_is(syntax::token_type_t::LBRACE)) {
        parser.advance();
        const auto raw_stmt{TRY(block_stmt::parse(parser, true))};
        return parser.add_expr<label_expr>(
            start_token, stdx::option<identifier_handle>{}, raw_stmt);
    }

    // `comptime name: { ... }` is a labeled block; any other `comptime name` starts an expression
    const bool labeled{[&] {
        if (!parser.peek_token_is(syntax::token_type_t::IDENT)) { return false; }
        const syntax::parser::transaction tx{parser};
        parser.advance();
        return parser.peek_token_is(syntax::token_type_t::COLON);
    }()};
    if (labeled) {
        parser.advance();
        const identifier_handle ident{TRY(identifier_expr::parse(parser))};
        TRY(parser.expect_peek(syntax::token_type_t::COLON));
        parser.advance();
        const syntax::parser::compile_time_scope cx_scope{parser, true};
        const auto                               raw_stmt{TRY(parser.parse_statement())};
        const auto body{TRY(label_expr::deconstruct_body(parser, raw_stmt))};
        if (!body.is<block_stmt>()) {
            return make_syntax_err("Comptime labels may only be applied to blocks",
                                   syntax::error::ILLEGAL_LABEL_STATEMENT,
                                   parser.get_location_of(*raw_stmt));
        }

        parser.get_ast().get_as_mut<block_stmt>(*body).is_comptime = true;
        return parser.add_expr<label_expr>(
            start_token, stdx::option<identifier_handle>{ident}, body);
    }

    return comptime_expr::parse(parser);
}

auto comptime_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    if (parser.peek_token_is(syntax::token_type_t::END) ||
        parser.peek_token_is(syntax::token_type_t::SEMICOLON)) {
        return make_syntax_err("Expected an expression after 'comptime'",
                               syntax::error::PREFIX_MISSING_OPERAND,
                               start_token);
    }
    if (parser.in_compile_time_position()) {
        return make_syntax_err(
            "Redundant 'comptime': this expression is already evaluated at compile time",
            syntax::error::REDUNDANT_COMPTIME,
            start_token);
    }
    parser.advance();

    const syntax::parser::compile_time_scope cx_scope{parser, true};
    const auto operand{TRY(parser.parse_expression(syntax::bind_precedence::PREFIX))};
    return parser.add_expr<comptime_expr>(start_token, operand);
}

auto label_expr::deconstruct_body(syntax::parser& parser, stmt_handle raw_stmt)
    -> stdx::result<labeled_node_handle, syntax::diagnostic> {
    switch (raw_stmt->get_kind()) {
    case node_kind::EXPRESSION_STATEMENT: {
        const auto& stmt{parser.get_node<expr_stmt>(*raw_stmt)};
        switch (stmt.expression->get_kind()) {
        case node_kind::FOR_LOOP_EXPRESSION:
            if (parser.get_node<for_loop_expr>(*stmt.expression).is_comptime) {
                return make_syntax_err("`for comptime` cannot be labeled; it has no `break`/"
                                       "`continue` to target",
                                       syntax::error::COMPTIME_LOOP_LABELED,
                                       parser.get_location_of(*raw_stmt));
            }
            return stmt.expression;
        case node_kind::WHILE_LOOP_EXPRESSION:
            if (parser.get_node<while_loop_expr>(*stmt.expression).is_comptime) {
                return make_syntax_err("`while comptime` cannot be labeled; it has no `break`/"
                                       "`continue` to target",
                                       syntax::error::COMPTIME_LOOP_LABELED,
                                       parser.get_location_of(*raw_stmt));
            }
            return stmt.expression;
        case node_kind::DO_WHILE_LOOP_EXPRESSION:
            if (parser.get_node<do_while_loop_expr>(*stmt.expression).is_comptime) {
                return make_syntax_err(
                    "`do ... while comptime` cannot be labeled; it has no `break`/"
                    "`continue` to target",
                    syntax::error::COMPTIME_LOOP_LABELED,
                    parser.get_location_of(*raw_stmt));
            }
            return stmt.expression;
        case node_kind::INFINITE_LOOP_EXPRESSION:
            if (parser.get_node<infinite_loop_expr>(*stmt.expression).is_comptime) {
                return make_syntax_err("`loop comptime` cannot be labeled; it has no `break`/"
                                       "`continue` to target",
                                       syntax::error::COMPTIME_LOOP_LABELED,
                                       parser.get_location_of(*raw_stmt));
            }
            return stmt.expression;
        case node_kind::IF_EXPRESSION:
        case node_kind::MATCH_EXPRESSION: return stmt.expression;
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

    // `match comptime` selects its live arm at compile time, like `if comptime`.
    bool is_comptime{false};
    if (parser.peek_token_is(syntax::token_type_t::COMPTIME)) {
        is_comptime = true;
        parser.advance();
    }

    // Conditions have to be surrounded by parentheses
    TRY(parser.expect_peek(syntax::token_type_t::LPAREN));
    parser.advance();
    if (parser.current_token_is(syntax::token_type_t::RPAREN)) {
        return make_syntax_err("Match expressions must have a condition",
                               syntax::error::MATCH_EXPR_MISSING_CONDITION,
                               start_token);
    }

    const auto matcher{TRY(parse_compile_time_expression(parser, is_comptime))};
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
    bool             arms_force_break{false};

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

        std::vector<match_pattern_handle> patterns;
        patterns.emplace_back(*pattern_opt);

        // An arm may list several comma-separated patterns before its `=>`.
        const bool is_catch_all_arm{catch_all_idx == arm_idx};
        bool       trailing_comma{false}; // Asks the formatter to break them one per line
        while (parser.peek_token_is(syntax::token_type_t::COMMA)) {
            parser.advance(); // current == COMMA
            if (parser.peek_token_is(syntax::token_type_t::FAT_ARROW)) {
                trailing_comma = true;
                break;
            }
            parser.advance(); // current == the extra pattern's first token
            if (is_catch_all_arm || parser.current_token_is(syntax::token_type_t::UNDERSCORE)) {
                return make_syntax_err("A catch-all '_' arm cannot list additional patterns",
                                       syntax::error::ILLEGAL_MATCH_CATCH_ALL,
                                       parser.get_current_token());
            }

            const auto extra_tok{parser.get_current_token()};
            const auto extra_raw{TRY(parser.parse_expression())};
            if (!match_pattern_handle::any_compatible(extra_raw->get_kind())) {
                return make_syntax_err(
                    fmt::format("Unmatchable expression '{}' used as a match arm pattern",
                                extra_raw->display_name()),
                    syntax::error::ILLEGAL_MATCH_PATTERN,
                    extra_tok);
            }
            patterns.emplace_back(extra_raw);
        }
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
        arms.emplace_back(std::move(patterns), capture, modifier, consequence, trailing_comma);
        arm_idx += 1;

        // The lack of a comma must mean we're at the end of the arm list
        if (parser.peek_token_is(syntax::token_type_t::COMMA)) {
            parser.advance();
            // A comma right before `}` is a trailing comma: keep one arm per line.
            arms_force_break = parser.peek_token_is(syntax::token_type_t::RBRACE);
        } else {
            break;
        }
    }

    TRY(parser.expect_peek(syntax::token_type_t::RBRACE));
    return parser.add_expr<match_expr>(
        start_token, matcher, std::move(arms), catch_all_idx, is_comptime, arms_force_break);
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
    // Pointer/reference types are formed over a named type, never a type literal in place
    constexpr bool forms_type{std::same_as<Expr, reference_expr> ||
                              std::same_as<Expr, address_of_expr>};
    if (forms_type && operand.template any<struct_expr, union_expr, enum_expr, interface_expr>()) {
        return make_syntax_err(
            "A struct, union, enum, or interface literal cannot take a type modifier",
            syntax::error::ILLEGAL_MODIFIED_TYPE_LITERAL,
            prefix_token);
    }
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
    const identifier_handle operand{TRY(parse_member_name(parser))};
    return parser.add_expr<implicit_access_expr>(prefix_token, operand);
}

auto string_expr::parse(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    return parser.add_expr<string_expr>(
        start_token, parser.get_ast().intern(start_token.materialize_string()), start_token.slice);
}

auto struct_expr::parse(syntax::parser& parser, bool is_extern, bool is_packed)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));
    std::vector<field>     fields;
    std::vector<cfg_group> cfg_groups;
    bool                   fields_force_break{false};
    while (!parser.peek_token_is(syntax::token_type_t::RBRACE) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        if (parser.peek_token_is(syntax::token_type_t::BUILTIN_CFG)) {
            if (cfg_group_targets_members(parser)) { break; } // handled by parse_members
            parser.advance();                                 // current == @cfg
            cfg_groups.emplace_back(
                TRY(parse_aggregate_cfg_group<field>(parser, fields.size(), parse_struct_field)));
            if (parser.peek_token_is(syntax::token_type_t::COMMA)) { parser.advance(); }
            continue;
        }

        const auto doc_floor{parser.get_current_token().line};
        bool       is_public{false};
        if (parser.peek_token_is(syntax::token_type_t::AT_LBRACKET) &&
            attributes_precede_member(parser)) {
            break;
        }
        auto attributes{TRY(try_parse_field_attributes(parser))};

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
        parser.attach_member_doc(ident, doc_floor);
        TRY(parser.expect_peek(syntax::token_type_t::COLON));
        const auto type{TRY(explicit_type::parse(parser))};

        stdx::option<expr_handle> value;
        if (parser.peek_token_is(syntax::token_type_t::ASSIGN)) {
            parser.advance(2);
            value.emplace(TRY(parser.parse_expression()));
        }

        // The identifier holds public information to save space in the field
        if (is_public) { ident->set_token_type(syntax::token_type_t::PUBLIC); }
        fields.emplace_back(ident, type, value, std::move(attributes));

        // No comma means that its the end or that there is a decl list starting
        const bool had_comma{parser.peek_token_is(syntax::token_type_t::COMMA)};
        if (had_comma) { parser.advance(); }
        parser.attach_member_doc(ident, doc_floor); // a trailing `///` is siphoned only now
        if (!had_comma) { break; }
        // A comma right before `}` is a trailing comma: keep one field per line.
        fields_force_break |= parser.peek_token_is(syntax::token_type_t::RBRACE);
    }

    std::vector<member_cfg_group> member_cfg_groups;
    auto                          members{TRY(parse_members(parser, member_cfg_groups))};
    TRY(parser.expect_peek(syntax::token_type_t::RBRACE));
    return parser.add_expr<struct_expr>(start_token,
                                        std::move(fields),
                                        std::move(cfg_groups),
                                        std::move(members),
                                        std::move(member_cfg_groups),
                                        is_extern,
                                        is_packed,
                                        fields_force_break);
}

auto union_expr::parse(syntax::parser& parser, bool is_extern, bool is_packed)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));
    std::vector<field>     fields;
    std::vector<cfg_group> cfg_groups;
    bool                   fields_force_break{false};
    while (!parser.peek_token_is(syntax::token_type_t::RBRACE) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        if (parser.peek_token_is(syntax::token_type_t::BUILTIN_CFG)) {
            if (cfg_group_targets_members(parser)) { break; } // handled by parse_members
            parser.advance();                                 // current == @cfg
            cfg_groups.emplace_back(
                TRY(parse_aggregate_cfg_group<field>(parser, fields.size(), parse_union_field)));
            if (parser.peek_token_is(syntax::token_type_t::COMMA)) { parser.advance(); }
            continue;
        }
        if (parser.peek_token_is(syntax::token_type_t::AT_LBRACKET)
                ? attributes_precede_member(parser)
                : parser.get_peek_token().is_member_token()) {
            break;
        }

        const auto doc_floor{parser.get_current_token().line};
        auto       attributes{TRY(try_parse_field_attributes(parser))};
        TRY(parser.expect_peek(syntax::token_type_t::IDENT));
        const identifier_handle ident{TRY(identifier_expr::parse(parser))};
        parser.attach_member_doc(ident, doc_floor);

        TRY(parser.expect_peek(syntax::token_type_t::COLON));
        const auto type{TRY(explicit_type::parse(parser))};

        fields.emplace_back(ident, type, std::move(attributes));

        // No comma means that its the end or that there is a decl list starting
        const bool had_comma{parser.peek_token_is(syntax::token_type_t::COMMA)};
        if (had_comma) { parser.advance(); }
        parser.attach_member_doc(ident, doc_floor); // a trailing `///` is siphoned only now
        if (!had_comma) { break; }
        // A comma right before `}` is a trailing comma: keep one field per line.
        fields_force_break |= parser.peek_token_is(syntax::token_type_t::RBRACE);
    }

    std::vector<member_cfg_group> member_cfg_groups;
    auto                          members{TRY(parse_members(parser, member_cfg_groups))};
    TRY(parser.expect_peek(syntax::token_type_t::RBRACE));

    // Validate here so that there aren't 3 errors spawning from an empty union with decls
    if (fields.empty() && cfg_groups.empty()) {
        return make_syntax_err("Unions must be declared with at least one field",
                               syntax::error::EMPTY_UNION,
                               start_token);
    }
    return parser.add_expr<union_expr>(start_token,
                                       std::move(fields),
                                       std::move(cfg_groups),
                                       std::move(members),
                                       std::move(member_cfg_groups),
                                       is_extern,
                                       is_packed,
                                       fields_force_break);
}

auto parse_modified_struct_or_union(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    const auto start_token{parser.get_current_token()};
    if (parser.current_token_is(syntax::token_type_t::EXTERN) &&
        parser.peek_token_is(syntax::token_type_t::FUNCTION)) {
        parser.advance();
        return function_expr::parse(parser, false, true);
    }

    bool is_extern{false};
    bool is_packed{false};

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
        return union_expr::parse(parser, is_extern, is_packed);
    }

    return make_syntax_err("Expected `struct` or `union` after declaration modifiers",
                           syntax::error::ILLEGAL_EXPLICIT_TYPE,
                           start_token);
}

auto while_loop_expr::parse(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};

    bool is_comptime{false};
    if (parser.peek_token_is(syntax::token_type_t::COMPTIME)) {
        parser.advance();
        is_comptime = true;
    }

    // Conditions have to be surrounded by parentheses
    TRY(parser.expect_peek(syntax::token_type_t::LPAREN));
    parser.advance();
    if (parser.current_token_is(syntax::token_type_t::RPAREN)) {
        return make_syntax_err("While loops must have a corresponding condition",
                               syntax::error::WHILE_MISSING_CONDITION,
                               start_token);
    }

    const auto condition{TRY(parse_compile_time_expression(parser, is_comptime))};
    TRY(parser.expect_peek(syntax::token_type_t::RPAREN));
    auto payload_capture{TRY(try_parse_capture(parser))};

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
    const block_handle  block{TRY(block_stmt::parse(parser))};
    capturing_alternate non_break;
    if (is_comptime) {
        if (parser.peek_token_is(syntax::token_type_t::ELSE)) {
            return make_syntax_err("`while comptime` cannot have an `else`/non-break clause",
                                   syntax::error::COMPTIME_LOOP_HAS_ELSE,
                                   parser.get_peek_token());
        }
    } else {
        non_break = TRY(try_parse_capturing_alternate(
            parser, payload_capture.has_value(), syntax::error::ILLEGAL_LOOP_NON_BREAK));
    }
    return parser.add_expr<while_loop_expr>(start_token,
                                            condition,
                                            continuation,
                                            block,
                                            non_break.body,
                                            is_comptime,
                                            std::move(payload_capture),
                                            std::move(non_break.capture));
}

auto parse_member_block(syntax::parser& parser) -> stdx::result<member_list, syntax::diagnostic> {
    PROFILE_FUNCTION();
    using tt = syntax::token_type_t;
    TRY(parser.expect_peek(tt::LBRACE));

    std::vector<member_cfg_group> member_cfg_groups;
    auto                          members{TRY(parse_members(parser, member_cfg_groups))};
    TRY(parser.expect_peek(tt::RBRACE));

    if (!member_cfg_groups.empty()) {
        return make_syntax_err("`@cfg` member groups are not supported inside `impl` blocks",
                               syntax::error::ILLEGAL_INTERFACE_MEMBER,
                               parser.get_current_token());
    }
    return members;
}

auto interface_expr::parse(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    using tt = syntax::token_type_t;
    const auto start_token{parser.get_current_token()};
    TRY(parser.expect_peek(tt::LBRACE));

    std::vector<assoc_type>  assoc_types;
    std::vector<assoc_const> assoc_consts;
    std::vector<method>      methods;

    while (!parser.peek_token_is(tt::RBRACE) && !parser.peek_token_is(tt::END)) {
        stdx::option<attribute_list> attributes;
        if (parser.peek_token_is(tt::AT_LBRACKET)) {
            parser.advance();
            attributes.emplace(TRY(parse_attribute_list(parser)));
        }

        bool is_pub{false};
        if (parser.peek_token_is(tt::PUBLIC)) {
            parser.advance();
            is_pub = true;
        }

        const auto is_method{parser.peek_token_is(tt::CONSTANT)};
        if (attributes && !is_method) {
            return make_syntax_err("Attributes may only be applied to interface methods",
                                   syntax::error::MISPLACED_ATTRIBUTES,
                                   parser.get_peek_token());
        }

        if (parser.peek_token_is(tt::CONSTANT)) {
            parser.advance(); // current == const
            TRY(parser.expect_peek(tt::IDENT));
            identifier_handle name{TRY(identifier_expr::parse(parser))};

            if (parser.peek_token_is(tt::ASSIGN)) {
                parser.advance(); // current == =
                TRY(parser.expect_peek(tt::FUNCTION));
                const auto signature{TRY(function_expr::parse(parser))};
                if (is_pub) { name->set_token_type(tt::PUBLIC); }
                methods.emplace_back(method{name, signature, std::move(attributes)});
            } else if (attributes) {
                return make_syntax_err("Attributes may only be applied to interface methods",
                                       syntax::error::MISPLACED_ATTRIBUTES,
                                       parser.get_current_token());
            } else if (parser.peek_token_is(tt::COLON)) {
                if (is_pub) {
                    return make_syntax_err("Associated `const`s may not be marked `pub`",
                                           syntax::error::ILLEGAL_INTERFACE_MEMBER,
                                           parser.get_current_token());
                }
                parser.advance(); // current == :
                const auto                annotation{TRY(explicit_type::parse(parser))};
                stdx::option<expr_handle> default_value;
                if (parser.peek_token_is(tt::ASSIGN)) {
                    parser.advance(2);
                    default_value.emplace(TRY(parser.parse_expression()));
                }
                assoc_consts.emplace_back(assoc_const{name, annotation, default_value});
            } else {
                return make_syntax_err(
                    "Interface members must be `const name = fn(...)` or `const N: T`",
                    syntax::error::ILLEGAL_INTERFACE_MEMBER,
                    parser.get_peek_token());
            }
            TRY(parser.expect_semicolon());
        } else if (!is_pub && parser.peek_token_is(tt::IDENT)) {
            parser.advance(); // current == Name
            identifier_handle name{TRY(identifier_expr::parse(parser))};
            TRY(parser.expect_peek(tt::COLON));
            const auto annotation{TRY(explicit_type::parse(parser))};

            stdx::option<explicit_type_id> default_type;
            if (parser.peek_token_is(tt::ASSIGN)) {
                parser.advance();
                default_type.emplace(TRY(explicit_type::parse(parser)));
            }
            assoc_types.emplace_back(assoc_type{name, annotation, default_type});
            TRY(parser.expect_semicolon());
        } else {
            return make_syntax_err("Illegal interface member",
                                   syntax::error::ILLEGAL_INTERFACE_MEMBER,
                                   parser.get_peek_token());
        }
    }

    TRY(parser.expect_peek(tt::RBRACE));
    return parser.add_expr<interface_expr>(
        start_token, std::move(assoc_types), std::move(assoc_consts), std::move(methods));
}

auto type_expr::parse_dyn(syntax::parser& parser) -> stdx::result<expr_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    if (auto fn_type{TRY(try_parse_dyn_fn(parser))}) {
        const auto type{parser.add_type<explicit_function_type>(
            start_token, type_modifier{}, std::move(*fn_type))};
        return parser.add_expr<type_expr>(start_token, type);
    }
    auto       dyn{TRY(explicit_dyn_type::parse(parser))};
    const auto type{
        parser.add_type<explicit_dyn_type>(start_token, type_modifier{}, std::move(dyn))};
    return parser.add_expr<type_expr>(start_token, type);
}

auto parameter_name(const AST& ast, const function_expr::parameter& param) -> std::string_view {
    if (!param.name.is<identifier_expr>()) { return {}; }
    return ast.get_as<identifier_expr>(param.name).name;
}

} // namespace ghoti::ast
