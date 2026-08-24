#include "driver/cmd/lsp/document_symbols.hh"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/statement.hh"
#include "compiler/module/module.hh"
#include "driver/cmd/lsp/diagnostics.hh"
#include "support/diagnostic.hh"

namespace ghoti::lsp {

namespace {

// There's no dedicated Union kind, so unions map to Struct
enum class symbol_kind : i32 {
    ENUM     = 10,
    FUNCTION = 12,
    VARIABLE = 13,
    CONSTANT = 14,
    STRUCT   = 23,
};

auto symbol_kind_of(const mod::module& module, const ast::decl_stmt& decl) -> symbol_kind {
    if (decl.value) {
        if (module.ast.get_as_opt<ast::function_expr>(*decl.value)) {
            return symbol_kind::FUNCTION;
        }
        if (module.ast.get_as_opt<ast::enum_expr>(*decl.value)) { return symbol_kind::ENUM; }
        if (module.ast.get_as_opt<ast::struct_expr>(*decl.value) ||
            module.ast.get_as_opt<ast::union_expr>(*decl.value)) {
            return symbol_kind::STRUCT;
        }
    }
    return decl.has_modifier(ast::decl_modifiers::CONSTANT) ? symbol_kind::CONSTANT
                                                            : symbol_kind::VARIABLE;
}

} // namespace

auto document_symbols(const mod::module& module) -> nlohmann::json {
    // Brace-init here would hit nlohmann's single-element-wraps-in-an-array pitfall
    auto out = nlohmann::json::array();

    for (const auto root_id : module.ast) {
        const auto decl{module.ast.get_as_opt<ast::decl_stmt>(root_id)};
        if (!decl) { continue; }
        const auto& name_ident{module.ast.get_as<ast::identifier_expr>(decl->name)};

        const source_span full_span{module.ast.location_of(root_id),
                                    module.ast.end_location_of(root_id)};
        const source_span name_span{module.ast.location_of(decl->name),
                                    module.ast.end_location_of(decl->name)};
        out.push_back({
            {"name", std::string{name_ident.name}},
            {"kind", std::to_underlying(symbol_kind_of(module, *decl))},
            {"range", range_of(full_span)},
            {"selectionRange", range_of(name_span)},
        });
    }
    return out;
}

} // namespace ghoti::lsp
