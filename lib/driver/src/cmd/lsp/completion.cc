#include "driver/cmd/lsp/completion.hh"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/statement.hh"
#include "compiler/module/module.hh"
#include "compiler/syntax/keywords.hh"

namespace ghoti::lsp {

namespace {

// LSP CompletionItemKind
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

} // namespace

auto completion_items(const mod::module& module) -> nlohmann::json {
    // Brace-init here would hit nlohmann's single-element-wraps-in-an-array pitfall
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

    return out;
}

} // namespace ghoti::lsp
