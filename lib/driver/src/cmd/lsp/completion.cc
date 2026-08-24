#include "driver/cmd/lsp/completion.hh"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/statement.hh"
#include "compiler/module/module.hh"
#include "compiler/syntax/keywords.hh"
#include "support/diagnostic.hh"

namespace ghoti::lsp {

namespace {

// There's no dedicated Union kind, so unions map to Struct
enum class completion_kind : i32 {
    FUNCTION = 3,
    VARIABLE = 6,
    KEYWORD  = 14,
    CONSTANT = 21,
    STRUCT   = 22,
    ENUM     = 13,
};

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

        const auto& ident{module.ast.get_as<ast::identifier_expr>(id)};
        out.push_back({
            {"label", std::string{ident.name}},
            {"kind", completion_kind::VARIABLE},
        });
    }

    return out;
}

} // namespace

auto completion_items(const mod::module& module, source_location target) -> nlohmann::json {
    auto out = nlohmann::json::array();

    for (const auto& keyword : syntax::ALL_KEYWORDS) {
        out.push_back({
            {"label", std::string{keyword.name}},
            {"kind", completion_kind::KEYWORD},
        });
    }

    for (const auto root_id : module.ast) {
        const auto decl{module.ast.get_as_opt<ast::decl_stmt>(root_id)};
        if (!decl) { continue; }
        const auto& name_ident{module.ast.get_as<ast::identifier_expr>(decl->name)};
        out.push_back({
            {"label", std::string{name_ident.name}},
            {"kind", std::to_underlying(completion_kind_of(module, *decl))},
        });
    }

    for (auto& item : local_scope_completions(module, target)) { out.push_back(std::move(item)); }
    return out;
}

} // namespace ghoti::lsp
