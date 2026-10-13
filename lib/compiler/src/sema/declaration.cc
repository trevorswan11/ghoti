#include "compiler/sema/declaration.hh"

#include <string_view>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/type.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/side_tables.hh"

namespace ghoti::sema {

namespace {

using names_t = std::vector<callable_param>;

// One declaration or annotation in one module. Visiting each once ends a chain that loops back
// on itself, like `const @"i32": i32 = ...` naming itself
struct visit {
    gsl::not_null<const mod::module_t*> owner;
    usize                               index;
    bool                                annotation;

    auto operator==(const visit&) const noexcept -> bool = default;
};

struct visit_hash {
    [[nodiscard]] static auto operator()(const visit& v) noexcept -> u64 {
        return stdx::hasher{reinterpret_cast<u64>(v.owner.get())}
            .combine(v.index)
            .combine(v.annotation)
            .finalize();
    }
};

using visited_t = ankerl::unordered_dense::set<visit, visit_hash>;

auto names_of(const declaration_ref& declaration, visited_t& visited) -> stdx::option<names_t>;

auto names_of_literal(const mod::module_t& owner, const ast::function_expr& fn) -> names_t {
    names_t names;
    if (fn.self) { names.emplace_back(owner.ast.get_as<ast::identifier_expr>(fn.self->name).name); }
    for (const auto& param : fn.parameters) {
        names.emplace_back(param.name.is<ast::identifier_expr>()
                               ? owner.ast.get_as<ast::identifier_expr>(param.name).name
                               : std::string_view{"_"},
                           param.default_value ? owner.source_of(*param.default_value)
                                               : std::string_view{});
    }
    return names;
}

auto names_of_annotation(const mod::module_t& owner, ast::explicit_type_id type, visited_t& visited)
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
        return names_of(*alias, visited);
    }
    return stdx::none;
}

// `const g = f;` / `const g = mod.f;`
auto names_of_alias_value(const mod::module_t& owner, ast::expr_handle value, visited_t& visited)
    -> stdx::option<names_t> {
    ast::node_id named{value};
    if (const auto dot{owner.ast.get_as_opt<ast::dot_expr>(value)}) { named = dot->member; }
    if (!owner.ast.get_as_opt<ast::identifier_expr>(named)) { return stdx::none; }
    if (const auto target{owner.get_identifier_declaration(named)}) {
        return names_of(*target, visited);
    }
    return stdx::none;
}

auto names_of(const declaration_ref& declaration, visited_t& visited) -> stdx::option<names_t> {
    if (!declaration.owner) { return stdx::none; }
    const auto& owner{*declaration.owner};
    if (declaration.annotation) {
        if (!visited.insert({&owner, declaration.annotation->get_index(), true}).second) {
            return stdx::none;
        }
        return names_of_annotation(owner, *declaration.annotation, visited);
    }
    if (!declaration.decl ||
        !visited.insert({&owner, declaration.decl->get_index(), false}).second) {
        return stdx::none;
    }

    const auto decl{owner.ast.get_as_opt<ast::decl_stmt>(*declaration.decl)};
    if (!decl) { return stdx::none; }
    if (decl->explicit_type) { return names_of_annotation(owner, *decl->explicit_type, visited); }
    if (!decl->value) { return stdx::none; }
    if (const auto fn{owner.ast.get_as_opt<ast::function_expr>(*decl->value)}) {
        return names_of_literal(owner, *fn);
    }

    // `const Op = dyn Fn(n: i32): i32;`
    if (const auto type_value{owner.ast.get_as_opt<ast::type_expr>(*decl->value)}) {
        return names_of_annotation(owner, type_value->type, visited);
    }
    return names_of_alias_value(owner, *decl->value, visited);
}

} // namespace

auto callable_params(const declaration_ref& declaration)
    -> stdx::option<std::vector<callable_param>> {
    visited_t visited;
    return names_of(declaration, visited);
}

auto callable_param_names(const declaration_ref& declaration)
    -> stdx::option<std::vector<std::string_view>> {
    const auto params{callable_params(declaration)};
    if (!params) { return stdx::none; }
    std::vector<std::string_view> names;
    names.reserve(params->size());
    for (const auto& param : *params) { names.emplace_back(param.name); }
    return names;
}

} // namespace ghoti::sema
