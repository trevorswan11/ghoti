#include "driver/cmd/lsp/completion.hh"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/statement.hh"
#include "compiler/module/module.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/keywords.hh"
#include "support/diagnostic.hh"

namespace ghoti::lsp {

namespace {

auto completion_kind_of(const mod::module& module, const ast::decl_stmt& decl) -> completion_kind {
    if (decl.value) {
        if (module.ast.get_as_opt<ast::function_expr>(*decl.value)) {
            return completion_kind::FUNCTION;
        }
        if (module.ast.get_as_opt<ast::enum_expr>(*decl.value)) { return completion_kind::ENUM; }
        if (module.ast.get_as_opt<ast::struct_expr>(*decl.value) ||
            module.ast.get_as_opt<ast::union_expr>(*decl.value)) {
            return completion_kind::STRUCT;
        }
    }
    return decl.has_modifier(ast::decl_modifiers::CONSTANT) ? completion_kind::CONSTANT
                                                            : completion_kind::VARIABLE;
}

auto at_or_before(source_location a, source_location b) -> bool {
    return a.line < b.line || (a.line == b.line && a.column <= b.column);
}

// source_span is half-open [start, end)
auto contains(source_span span, source_location point) -> bool {
    return at_or_before(span.start, point) &&
           (point.line < span.end.line ||
            (point.line == span.end.line && point.column < span.end.column));
}

// Every declared name that's in scope at `target`
auto local_scope_completions(const mod::module& module, source_location target) -> nlohmann::json {
    auto out = nlohmann::json::array();

    stdx::option<source_span> enclosing;
    for (const auto root_id : module.ast) {
        const source_span span{module.ast.location_of(root_id),
                               module.ast.end_location_of(root_id)};
        if (contains(span, target)) {
            enclosing = span;
            break;
        }
    }
    if (!enclosing) { return out; }

    for (const auto id : module.identifier_positions) {
        if (module.get_identifier_definition(id)) { continue; } // a reference, not a declaration
        const auto start{module.ast.location_of(id)};
        if (!contains(*enclosing, start) || !at_or_before(start, target)) { continue; }

        const auto ident{module.ast.get_as_opt<ast::identifier_expr>(id)};
        if (!ident) { continue; }
        out.push_back({
            {"label", std::string{ident->name}},
            {"kind", completion_kind::VARIABLE},
        });
    }

    return out;
}

[[nodiscard]] auto is_word_char(char c) -> bool {
    return std::isalnum(static_cast<u8>(c)) != 0 || c == '_';
}

// Byte offset of a zero-based line and column, clamped to the end of that line
[[nodiscard]] auto offset_of(std::string_view source, source_location target) -> usize {
    usize offset{0};
    for (usize line{0}; line < target.line; ++line) {
        const auto newline{source.find('\n', offset)};
        if (newline == std::string_view::npos) { return source.size(); }
        offset = newline + 1;
    }
    const auto line_end{std::min(source.find('\n', offset), source.size())};
    return std::min(offset + target.column, line_end);
}

[[nodiscard]] auto attribute_completions(const attribute_context& context) -> nlohmann::json {
    auto out = nlohmann::json::array();
    if (context.in_args_of) {
        if (!context.after_dot) { return out; }
        for (const auto variant : ast::attribute_enum_variants(*context.in_args_of)) {
            out.push_back({{"label", std::string{variant}}, {"kind", completion_kind::MEMBER}});
        }
        return out;
    }
    for (const auto& spec : ast::all_attribute_specs()) {
        out.push_back({
            {"label", std::string{spec.name}},
            {"kind", completion_kind::PROPERTY},
            {"detail", std::string{spec.signature}},
            {"documentation", std::string{spec.doc}},
        });
    }
    return out;
}

} // namespace

auto attribute_context_at(std::string_view source, source_location target)
    -> stdx::option<attribute_context> {
    const auto cursor{offset_of(source, target)};

    // The word under the cursor extends both ways, so hover sees all of it
    usize word_start{cursor};
    while (word_start > 0 && is_word_char(source[word_start - 1])) { --word_start; }
    usize word_end{cursor};
    while (word_end < source.size() && is_word_char(source[word_end])) { ++word_end; }

    const auto open{source.rfind("@[", word_start)};
    if (open == std::string_view::npos) { return stdx::none; }

    // Replay the list up to the word, skipping strings, to see what is still open
    usize                             depth{0}, last_name_start{0}, last_name_end{0};
    stdx::option<ast::attribute_kind> in_args_of;
    for (usize i{open + 2}; i < word_start; ++i) {
        const auto c{source[i]};
        if (c == '"') {
            for (++i; i < word_start && source[i] != '"'; ++i) {
                if (source[i] == '\\') { ++i; }
            }
            continue;
        }
        if (c == ']' && depth == 0) { return stdx::none; }
        if (c == ';' || c == '{' || c == '}') { return stdx::none; }
        if (c == '(') {
            if (depth++ == 0) {
                const auto name{source.substr(last_name_start, last_name_end - last_name_start)};
                const auto spec{ast::attribute_spec_of(name)};
                in_args_of = spec ? stdx::option<ast::attribute_kind>{spec->kind} : stdx::none;
            }
        } else if (c == ')') {
            if (depth > 0 && --depth == 0) { in_args_of = stdx::none; }
        } else if (is_word_char(c) && (i == 0 || !is_word_char(source[i - 1]))) {
            last_name_start = i;
            last_name_end   = i;
            while (last_name_end < word_start && is_word_char(source[last_name_end])) {
                ++last_name_end;
            }
        }
    }

    return attribute_context{
        .in_args_of = depth > 0 ? in_args_of : stdx::none,
        .word       = source.substr(word_start, word_end - word_start),
        .after_dot  = word_start > 0 && source[word_start - 1] == '.',
    };
}

auto completion_items(const mod::module& module, source_location target) -> nlohmann::json {
    if (const auto context{attribute_context_at(std::string_view{module.source}, target)}) {
        return attribute_completions(*context);
    }

    auto out = nlohmann::json::array();

    for (const auto& keyword : syntax::ALL_KEYWORDS) {
        out.push_back({
            {"label", std::string{keyword.name}},
            {"kind", completion_kind::KEYWORD},
        });
    }

    for (const auto& builtin : syntax::ALL_BUILTINS) {
        out.push_back({
            {"label", std::string{builtin.name}},
            {"kind", completion_kind::FUNCTION},
        });
    }

    for (const auto root_id : module.ast) {
        const auto decl{module.ast.get_as_opt<ast::decl_stmt>(root_id)};
        if (!decl) { continue; }
        const auto name_ident{module.ast.get_as_opt<ast::identifier_expr>(decl->name)};
        if (!name_ident) { continue; }
        out.push_back({
            {"label", std::string{name_ident->name}},
            {"kind", std::to_underlying(completion_kind_of(module, *decl))},
        });
    }

    for (auto& item : local_scope_completions(module, target)) { out.push_back(std::move(item)); }
    return out;
}

} // namespace ghoti::lsp
