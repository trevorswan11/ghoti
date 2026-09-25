#include "compiler/sema/declaration.hh"

#include <string_view>
#include <vector>

#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/type.hh"
#include "compiler/module/module.hh"

namespace ghoti::sema {

namespace {

using names_t = std::vector<std::string_view>;

// `const @"i32": i32 = ...` names itself, so every alias chain is bounded
constexpr u32 MAX_ALIAS_DEPTH{8};

auto names_of(const declaration_ref& declaration, u32 depth) -> stdx::option<names_t>;

auto names_of_literal(const mod::module& owner, const ast::function_expr& fn) -> names_t {
    names_t names;
    if (fn.self) { names.emplace_back(owner.ast.get_as<ast::identifier_expr>(fn.self->name).name); }
    for (const auto& param : fn.parameters) {
        names.emplace_back(param.name.is<ast::identifier_expr>()
                               ? owner.ast.get_as<ast::identifier_expr>(param.name).name
                               : std::string_view{"_"});
    }
    return names;
}

auto names_of_annotation(const mod::module& owner, ast::explicit_type_id type, u32 depth)
    -> stdx::option<names_t> {
    if (!type.is_valid()) { return stdx::none; }
    if (const auto fn_type{owner.ast.get_as_opt<ast::explicit_function_type>(type)}) {
        names_t names;
        for (const auto name : fn_type->parameter_names) {
            names.emplace_back(owner.ast.get_as<ast::identifier_expr>(name).name);
        }
        return names;
    }
    // `f: Callback` / `f: mod.Callback`
    if (const auto alias{owner.get_identifier_declaration(type)}) {
        return names_of(*alias, depth + 1);
    }
    return stdx::none;
}

// `const g := f;` / `const g := mod.f;`
auto names_of_alias_value(const mod::module& owner, ast::expr_handle value, u32 depth)
    -> stdx::option<names_t> {
    ast::node_id named{value};
    if (const auto dot{owner.ast.get_as_opt<ast::dot_expr>(value)}) { named = dot->member; }
    if (!owner.ast.get_as_opt<ast::identifier_expr>(named)) { return stdx::none; }
    if (const auto target{owner.get_identifier_declaration(named)}) {
        return names_of(*target, depth + 1);
    }
    return stdx::none;
}

auto names_of(const declaration_ref& declaration, u32 depth) -> stdx::option<names_t> {
    if (depth >= MAX_ALIAS_DEPTH || !declaration.owner) { return stdx::none; }
    const auto& owner{*declaration.owner};
    if (declaration.annotation) {
        return names_of_annotation(owner, *declaration.annotation, depth);
    }
    if (!declaration.decl) { return stdx::none; }

    const auto decl{owner.ast.get_as_opt<ast::decl_stmt>(*declaration.decl)};
    if (!decl) { return stdx::none; }
    if (decl->explicit_type) { return names_of_annotation(owner, *decl->explicit_type, depth); }
    if (!decl->value) { return stdx::none; }
    if (const auto fn{owner.ast.get_as_opt<ast::function_expr>(*decl->value)}) {
        return names_of_literal(owner, *fn);
    }
    // `const Op := dyn Fn(n: i32): i32;`
    if (const auto type_value{owner.ast.get_as_opt<ast::type_expr>(*decl->value)}) {
        return names_of_annotation(owner, type_value->type, depth);
    }
    return names_of_alias_value(owner, *decl->value, depth);
}

} // namespace

auto callable_param_names(const declaration_ref& declaration) -> stdx::option<names_t> {
    return names_of(declaration, 0);
}

} // namespace ghoti::sema
