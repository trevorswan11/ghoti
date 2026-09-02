#include "compiler/ast/statement.hh"

#include <bit>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <stdx/fixed/enum_map.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>

#include "compiler/ast/ast.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/type.hh"
#include "compiler/syntax/error.hh"
#include "compiler/syntax/parser.hh"
#include "compiler/syntax/token_type.hh"

namespace ghoti::ast {

auto block_stmt::parse(syntax::parser& parser) -> stdx::result<stmt_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};

    statements_t statements;
    while (!parser.peek_token_is(syntax::token_type_t::RBRACE) &&
           !parser.peek_token_is(syntax::token_type_t::END)) {
        parser.advance();
        statements.emplace_back(TRY(parser.parse_statement()));
    }
    TRY(parser.expect_peek(syntax::token_type_t::RBRACE));

    return parser.add_stmt<block_stmt>(start_token, std::move(statements));
}

auto break_stmt::parse(syntax::parser& parser) -> stdx::result<stmt_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};

    // Labels are optional
    stdx::option<identifier_handle> label;
    if (parser.peek_token_is(syntax::token_type_t::COLON)) {
        parser.advance();
        TRY(parser.expect_peek(syntax::token_type_t::IDENT));
        label.emplace(TRY(identifier_expr::parse(parser)));
    }

    // Values can be present but must be associated with a label
    stdx::option<expr_handle> value;
    if (!parser.peek_token_is(syntax::token_type_t::END) &&
        !parser.peek_token_is(syntax::token_type_t::SEMICOLON)) {
        parser.advance();
        value.emplace(TRY(parser.parse_expression()));
    }

    if (value && !label) {
        return make_syntax_err("Valued break statements must be labeled",
                               syntax::error::VALUED_BREAK_MISSING_LABEL,
                               start_token);
    }
    TRY(parser.expect_semicolon());
    return parser.add_stmt<break_stmt>(start_token, label, value);
}

namespace {

// Parses a `@cfg` arm body: either a braced `{ <item>* }` group or a single bare item
[[nodiscard]] auto parse_cfg_body(syntax::parser& parser)
    -> stdx::result<std::vector<stmt_handle>, syntax::diagnostic> {
    using syntax::token_type_t;
    std::vector<stmt_handle> items;

    if (parser.current_token_is(token_type_t::LBRACE)) {
        while (!parser.peek_token_is(token_type_t::RBRACE) &&
               !parser.peek_token_is(token_type_t::END)) {
            parser.advance();
            if (parser.current_token_is(token_type_t::SEMICOLON)) { continue; }
            items.emplace_back(TRY(parser.parse_statement()));
        }
        TRY(parser.expect_peek(token_type_t::RBRACE));
    } else {
        items.emplace_back(TRY(parser.parse_statement()));
    }
    return items;
}

} // namespace

auto cfg_stmt::parse(syntax::parser& parser) -> stdx::result<stmt_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    using syntax::token_type_t;
    const auto start_token{parser.get_current_token()};

    std::vector<arm> arms;

    const auto predicate{TRY(parser.parse_cfg_predicate())};
    parser.advance();
    arms.emplace_back(arm{stdx::option<expr_handle>{predicate}, TRY(parse_cfg_body(parser))});

    while (parser.peek_token_is(token_type_t::ELSE)) {
        parser.advance(); // current = else
        if (parser.peek_token_is(token_type_t::BUILTIN_CFG)) {
            parser.advance(); // current = @cfg
            const auto else_predicate{TRY(parser.parse_cfg_predicate())};
            parser.advance();
            arms.emplace_back(
                arm{stdx::option<expr_handle>{else_predicate}, TRY(parse_cfg_body(parser))});
        } else {
            parser.advance(); // current = body's first token
            arms.emplace_back(arm{stdx::none, TRY(parse_cfg_body(parser))});
            break; // a predicate-less `else` terminates the chain
        }
    }

    return parser.add_stmt<cfg_stmt>(start_token, std::move(arms));
}

auto continue_stmt::parse(syntax::parser& parser) -> stdx::result<stmt_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};

    // Labels are optional
    stdx::option<identifier_handle> label;
    if (parser.peek_token_is(syntax::token_type_t::COLON)) {
        parser.advance();
        TRY(parser.expect_peek(syntax::token_type_t::IDENT));
        label.emplace(TRY(identifier_expr::parse(parser)));
    }

    // Values can never be present in a continue
    if (!parser.peek_token_is(syntax::token_type_t::END) &&
        !parser.peek_token_is(syntax::token_type_t::SEMICOLON)) {
        return make_syntax_err("Continue statements may only contain labels",
                               syntax::error::VALUED_CONTINUE,
                               start_token);
    }

    TRY(parser.expect_peek(syntax::token_type_t::SEMICOLON));
    return parser.add_stmt<continue_stmt>(start_token, label);
}

namespace {

using modifier_mapping = std::pair<syntax::token_type_t, decl_modifiers>;
constexpr auto LEGAL_MODIFIERS{
    stdx::fixed::enum_map<syntax::token_type_t, stdx::option<decl_modifiers>>::from(
        stdx::none,
        modifier_mapping{syntax::token_type_t::VAR, decl_modifiers::VARIABLE},
        modifier_mapping{syntax::token_type_t::CONSTANT, decl_modifiers::CONSTANT},
        modifier_mapping{syntax::token_type_t::CONSTEXPR, decl_modifiers::CONSTEXPR},
        modifier_mapping{syntax::token_type_t::PUBLIC, decl_modifiers::PUBLIC},
        modifier_mapping{syntax::token_type_t::EXTERN, decl_modifiers::EXTERN},
        modifier_mapping{syntax::token_type_t::EXPORT, decl_modifiers::EXPORT},
        modifier_mapping{syntax::token_type_t::THREADLOCAL, decl_modifiers::THREADLOCAL},
        modifier_mapping{syntax::token_type_t::WEAK, decl_modifiers::WEAK},
        modifier_mapping{syntax::token_type_t::BUILTIN_DISCARDABLE, decl_modifiers::DISCARDABLE})};

[[nodiscard]] constexpr auto validate_modifiers(decl_modifiers modifiers) noexcept
    -> stdx::option<std::string> {
    const auto mut_count{std::popcount(
        std::to_underlying(modifiers & (decl_modifiers::VARIABLE | decl_modifiers::CONSTANT |
                                        decl_modifiers::CONSTEXPR)))};
    if (mut_count != 1) {
        return fmt::format("Exactly one mutability modifier may be used; found {}", mut_count);
    }

    const auto valid_constexpr{
        std::popcount(std::to_underlying(
            modifiers & (decl_modifiers::EXTERN | decl_modifiers::CONSTEXPR))) <= 1};
    if (!valid_constexpr) { return "Extern values cannot be known at compile time"; }

    const auto abi_count{std::popcount(
        std::to_underlying(modifiers & (decl_modifiers::EXTERN | decl_modifiers::EXPORT)))};
    if (abi_count > 1) {
        return fmt::format("At most one ABI-related modifier may be used; found {}", abi_count);
    }

    const auto tls_constexpr{decl_modifiers::THREADLOCAL | decl_modifiers::CONSTEXPR};
    if ((modifiers & tls_constexpr) == tls_constexpr) {
        return "A 'threadlocal' declaration cannot also be 'constexpr'";
    }

    const auto weak_constexpr{decl_modifiers::WEAK | decl_modifiers::CONSTEXPR};
    if ((modifiers & weak_constexpr) == weak_constexpr) {
        return "A 'weak' declaration cannot also be 'constexpr'";
    }
    return stdx::none;
}

struct binding_args {
    stdx::option<string_handle> first;  // extern: library; export: exported name
    stdx::option<string_handle> second; // extern: link name; unused for export
};

// Parses an optional `("a")` or `("a", "b")` suffix after an `extern`/`export` token.
[[nodiscard]] auto try_parse_binding_args(syntax::parser& parser, bool allow_second)
    -> stdx::result<binding_args, syntax::diagnostic> {
    binding_args out;
    if (!parser.peek_token_is(syntax::token_type_t::LPAREN)) { return out; }

    const auto parse_string{
        [&](std::string_view what) -> stdx::result<string_handle, syntax::diagnostic> {
            TRY(parser.expect_peek(syntax::token_type_t::STRING));
            const auto handle{TRY(string_expr::parse(parser))};
            if (parser.get_node<string_expr>(*handle).value.empty()) {
                return make_syntax_err(fmt::format("{} may not be empty", what),
                                       syntax::error::EMPTY_EXTERN_TARGET,
                                       parser.get_location_of(*handle));
            }
            return handle;
        }};

    parser.advance();
    out.first.emplace(TRY(parse_string(allow_second ? "Extern target" : "Exported name")));
    if (allow_second && parser.peek_token_is(syntax::token_type_t::COMMA)) {
        parser.advance();
        out.second.emplace(TRY(parse_string("Link name")));
    }
    TRY(parser.expect_peek(syntax::token_type_t::RPAREN));
    return out;
}

} // namespace

auto decl_stmt::parse(syntax::parser& parser) -> stdx::result<stmt_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    auto       modifiers{LEGAL_MODIFIERS[start_token.type].value()};

    stdx::option<string_handle> extern_target;
    stdx::option<string_handle> link_name;

    const auto parse_binding_for{[&](decl_modifiers m) -> stdx::result<void, syntax::diagnostic> {
        if (m == decl_modifiers::EXTERN) {
            auto args{TRY(try_parse_binding_args(parser, true))};
            extern_target = args.first;
            if (args.second) { link_name = args.second; }
        } else if (m == decl_modifiers::EXPORT) {
            auto args{TRY(try_parse_binding_args(parser, false))};
            if (args.first) { link_name = args.first; }
        }
        return {};
    }};

    TRY(parse_binding_for(modifiers));

    stdx::option<decl_modifiers> current_modifier;
    while ((current_modifier = LEGAL_MODIFIERS[parser.get_peek_token().type])) {
        parser.advance();
        if (modifiers_has(modifiers, *current_modifier)) {
            return make_syntax_err("Declaration modifiers may only be used once in any order",
                                   syntax::error::DUPLICATE_DECL_MODIFIER,
                                   parser.get_current_token());
        }
        modifiers |= *current_modifier;

        TRY(parse_binding_for(*current_modifier));
    }

    if (auto msg{validate_modifiers(modifiers)}) {
        return make_syntax_err(
            std::move(msg).value(), syntax::error::ILLEGAL_DECL_MODIFIERS, start_token);
    }

    TRY(parser.expect_peek(syntax::token_type_t::IDENT));
    const identifier_handle decl_name{TRY(identifier_expr::parse(parser))};
    const auto [decl_type, value_initialized]{TRY(explicit_type::parse_opt_init(parser))};

    stdx::option<expr_handle> decl_value;
    if (value_initialized) {
        decl_value.emplace(TRY(parser.parse_expression()));

        // If there is a value, then there cannot be an extern due to a contradiction
        if (modifiers_has(modifiers, decl_modifiers::EXTERN)) {
            return make_syntax_err("Extern declarations may not be value-initialized",
                                   syntax::error::EXTERN_VALUE_INITIALIZED,
                                   start_token);
        }
    } else if (!modifiers_has(modifiers, decl_modifiers::EXTERN)) {
        return make_syntax_err(
            "Non-extern declarations must be value-initialized; use '= undefined' to leave a "
            "'var' unspecified",
            syntax::error::DECL_MISSING_VALUE,
            start_token);
    }

    TRY(parser.expect_semicolon());
    return parser.add_stmt<decl_stmt>(
        start_token, decl_name, decl_type, decl_value, modifiers, extern_target, link_name);
}

auto defer_stmt::parse(syntax::parser& parser) -> stdx::result<stmt_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    if (parser.peek_token_is(syntax::token_type_t::END) ||
        parser.peek_token_is(syntax::token_type_t::SEMICOLON)) {
        return make_syntax_err("Defer statements require an statement to defer",
                               syntax::error::DEFER_MISSING_DEFERREE,
                               start_token);
    }
    parser.advance();
    const auto stmt{TRY(parser.parse_statement())};

    // The statement has different restrictions from expression alternates
    if (!stmt.any<expr_stmt, discard_stmt, block_stmt>()) {
        return make_syntax_err("Deferred statements must be expressions, discards, or blocks",
                               syntax::error::ILLEGAL_DEFERRED_STATEMENT,
                               parser.get_location_of(*stmt));
    }
    return parser.add_stmt<defer_stmt>(start_token, stmt);
}

auto discard_stmt::parse(syntax::parser& parser) -> stdx::result<stmt_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};

    TRY(parser.expect_peek(syntax::token_type_t::ASSIGN));
    if (parser.peek_token_is(syntax::token_type_t::END) ||
        parser.peek_token_is(syntax::token_type_t::SEMICOLON)) {
        return make_syntax_err("Discarded statements must have a statement to discard",
                               syntax::error::DISCARD_MISSING_DISCARDEE,
                               parser.get_current_token());
    }

    parser.advance();
    const auto expr{TRY(parser.parse_expression())};

    TRY(parser.expect_semicolon());
    return parser.add_stmt<discard_stmt>(start_token, expr);
}

auto expr_stmt::parse(syntax::parser& parser, syntax::semicolon_behavior behavior)
    -> stdx::result<stmt_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    const auto expr{TRY(parser.parse_expression())};

    using behavior_t = syntax::semicolon_behavior;
    const auto has_semicolon{parser.current_token_is(syntax::token_type_t::SEMICOLON)};
    const auto at_block_end{parser.current_token_is(syntax::token_type_t::RBRACE)};

    const auto check_illegal_semicolon = [&] -> stdx::option<stdx::err<syntax::diagnostic>> {
        const auto semicolon{parser.advance()};
        if (behavior == behavior_t::DISALLOW) {
            return syntax::make_syntax_err("Semicolon is not allowed in this context",
                                           syntax::error::UNEXPECTED_TOKEN,
                                           semicolon);
        }
        return stdx::none;
    };

    auto terminated{false};

    // RBRACE would mean we're at the end of a block and a semicolon is never required
    if (at_block_end) {
        if (parser.peek_token_is(syntax::token_type_t::SEMICOLON)) {
            if (auto err{check_illegal_semicolon()}) { return std::move(*err); }
            terminated = true;
        }
    } else if (has_semicolon) {
        terminated = true;
    } else {
        if (parser.peek_token_is(syntax::token_type_t::SEMICOLON)) {
            if (auto err{check_illegal_semicolon()}) { return std::move(*err); }
            terminated = true;
        } else if (behavior == behavior_t::REQUIRE) {
            TRY(parser.expect_peek(syntax::token_type_t::SEMICOLON));
            terminated = true;
        }
    }
    return parser.add_stmt<expr_stmt>(start_token, expr, terminated);
}

namespace {

[[nodiscard]] auto parse_import_payload(syntax::parser& parser)
    -> stdx::result<import_payload_handle, syntax::diagnostic> {
    if (parser.peek_token_is(syntax::token_type_t::IDENT)) {
        TRY(parser.expect_peek(syntax::token_type_t::IDENT));
        return TRY(identifier_expr::parse(parser));
    }

    if (parser.peek_token_is(syntax::token_type_t::STRING)) {
        TRY(parser.expect_peek(syntax::token_type_t::STRING));
        const string_handle string{TRY(string_expr::parse(parser))};

        if (parser.get_node<string_expr>(*string).value.empty()) {
            return make_syntax_err("File import names cannot be empty",
                                   syntax::error::EMPTY_FILE_IMPORT,
                                   parser.get_location_of(*string));
        }
        return string;
    }

    return make_syntax_err("Imported payloads may only be filename strings or module identifiers",
                           syntax::error::ILLEGAL_IMPORT_TYPE,
                           parser.get_peek_token());
}

} // namespace

auto import_stmt::parse(syntax::parser& parser) -> stdx::result<stmt_handle, syntax::diagnostic> {
    // A start token of public is guaranteed to be followed by an import
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    if (parser.current_token_is(syntax::token_type_t::PUBLIC)) { parser.advance(); }
    auto imported_core{TRY(parse_import_payload(parser))};

    stdx::option<identifier_handle> imported_alias;
    if (parser.peek_token_is(syntax::token_type_t::AS)) {
        parser.advance();
        TRY(parser.expect_peek(syntax::token_type_t::IDENT));

        imported_alias.emplace(TRY(identifier_expr::parse(parser)));
    } else if (imported_core->get_kind() == node_kind::STRING_EXPRESSION) {
        return make_syntax_err("All file imports must be aliased to an identifier",
                               syntax::error::FILE_IMPORT_MISSING_ALIAS,
                               start_token);
    }

    TRY(parser.expect_semicolon());
    return parser.add_stmt<import_stmt>(start_token, imported_core, imported_alias);
}

auto import_stmt::get_name(const AST& tree) const noexcept
    -> std::pair<ast::identifier_handle, std::string_view> {
    if (alias) {
        const auto  handle{*alias};
        const auto& ident{tree.get_as<ast::identifier_expr>(handle)};
        return {handle, ident.name};
    }

    // This must be an ident as all strings have an alias
    const auto& ident{tree.get_as<ast::identifier_expr>(payload)};
    return {payload, ident.name};
}

auto return_stmt::parse(syntax::parser& parser) -> stdx::result<stmt_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};

    stdx::option<expr_handle> value;
    if (!parser.peek_token_is(syntax::token_type_t::END) &&
        !parser.peek_token_is(syntax::token_type_t::SEMICOLON)) {
        parser.advance();
        value.emplace(TRY(parser.parse_expression()));
    }

    TRY(parser.expect_semicolon());
    return parser.add_stmt<return_stmt>(start_token, value);
}

auto test_stmt::parse(syntax::parser& parser) -> stdx::result<stmt_handle, syntax::diagnostic> {
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};

    stdx::option<string_handle> description;
    if (parser.peek_token_is(syntax::token_type_t::STRING)) {
        parser.advance();
        description.emplace(TRY(string_expr::parse(parser)));

        // Empty strings aren't supported since one should just use no description
        if (parser.get_node<string_expr>(**description).value.empty()) {
            return make_syntax_err("Test descriptions may not be empty when present",
                                   syntax::error::EMPTY_TEST_DESCRIPTION,
                                   parser.get_location_of(**description));
        }
    }

    TRY(parser.expect_peek(syntax::token_type_t::LBRACE));
    const block_handle block{TRY(block_stmt::parse(parser))};
    return parser.add_stmt<test_stmt>(start_token, description, block);
}

auto using_stmt::parse(syntax::parser& parser) -> stdx::result<stmt_handle, syntax::diagnostic> {
    // A start token of public is guaranteed to be followed by an import
    PROFILE_FUNCTION();
    const auto start_token{parser.get_current_token()};
    if (parser.current_token_is(syntax::token_type_t::PUBLIC)) { parser.advance(); }

    TRY(parser.expect_peek(syntax::token_type_t::IDENT));
    const identifier_handle alias{TRY(identifier_expr::parse(parser))};

    TRY(parser.expect_peek(syntax::token_type_t::ASSIGN));
    const auto type{TRY(explicit_type::parse(parser))};

    TRY(parser.expect_peek(syntax::token_type_t::SEMICOLON));
    return parser.add_stmt<using_stmt>(start_token, alias, type);
}

} // namespace ghoti::ast
