#include "compiler/gir/symbol_scoping.hh"

#include <string>
#include <string_view>

#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include <gsl/span>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "compiler/ast/ast.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/statement.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/type.hh"

namespace ghoti::gir {

namespace {

// Separator that cannot appear in a ghoti identifier, keeping composite keys unambiguous.
constexpr char SEP{'\x1f'};

using name_count_map_t = ankerl::unordered_dense::
    map<std::string, u32, stdx::string_transparent_hash, stdx::string_transparent_eq>;

struct scan_state {
    symbol_scoping::str_set_t& scopable_defs;
    name_count_map_t&          name_counts;
    symbol_scoping::str_set_t& weak_names;
    const sema::context&       ctx;
    const mod::module&         entry_mod;
};

// A function / `var` global whose name is fixed by an external contract must never be renamed.
[[nodiscard]] auto is_externally_fixed(const ast::decl_stmt& decl) -> bool {
    if (decl.has_modifier(ast::decl_modifiers::EXTERN)) { return true; }
    if (decl.has_modifier(ast::decl_modifiers::EXPORT)) { return true; }
    if (decl.has_modifier(ast::decl_modifiers::WEAK)) { return true; }
    return static_cast<bool>(decl.link_name);
}

auto scan_decl(scan_state&           st,
               const mod::module&    owner_mod,
               usize                 owner_table_idx,
               const ast::decl_stmt& decl) -> void {
    const auto& ast{owner_mod.ast};
    if (!decl.value) { return; }

    const auto name_ident{ast.get_as_opt<ast::identifier_expr>(decl.name)};
    if (!name_ident) { return; }
    const auto name{name_ident->name};

    const bool is_fn{ast.get_as_opt<ast::function_expr>(*decl.value).has_value()};
    const bool is_var{decl.has_modifier(ast::decl_modifiers::VARIABLE)};

    if (is_fn || is_var) {
        st.name_counts[std::string{name}] += 1;

        // A `weak` definition and its non-weak override are meant to collapse onto one symbol
        if (decl.has_modifier(ast::decl_modifiers::WEAK)) { st.weak_names.emplace(name); }

        const bool is_entry{&owner_mod == &st.entry_mod &&
                            owner_table_idx == *owner_mod.root_table_idx &&
                            name == st.ctx.user_main_name};
        if (!is_entry && !is_externally_fixed(decl)) {
            st.scopable_defs.emplace(fmt::format("{}{}{}", owner_table_idx, SEP, name));
        }
    }

    // Recurse into aggregate members, which live in the aggregate literal's own symbol table.
    stdx::option<const ast::member_list&> members;
    if (const auto s{ast.get_as_opt<ast::struct_expr>(*decl.value)}) {
        members.emplace(s->members);
    } else if (const auto u{ast.get_as_opt<ast::union_expr>(*decl.value)}) {
        members.emplace(u->members);
    } else if (const auto e{ast.get_as_opt<ast::enum_expr>(*decl.value)}) {
        members.emplace(e->members);
    }
    if (!members) { return; }

    const auto agg_type{owner_mod.get_sema_type_opt(*decl.value)};
    if (!agg_type) { return; }
    const auto agg_table{agg_type->get_symbol_table_idx_opt()};
    if (!agg_table) { return; }

    for (const auto& member : *members) {
        if (const auto md{ast.get_as_opt<ast::decl_stmt>(*member)}) {
            scan_decl(st, owner_mod, *agg_table, *md);
        }
    }
}

auto scan_module(scan_state& st, const mod::module& mod) -> void {
    if (!mod.root_table_idx || mod.is_errored() || mod.is_poisoned()) { return; }
    for (const auto root_id : mod.ast) {
        if (const auto decl{mod.ast.get_as_opt<ast::decl_stmt>(root_id)}) {
            scan_decl(st, mod, *mod.root_table_idx, *decl);
        }
    }
}

} // namespace

auto symbol_scoping::key_of(usize owner_table_idx, std::string_view name) -> std::string {
    PROFILE_FUNCTION();
    return fmt::format("{}{}{}", owner_table_idx, SEP, name);
}

auto symbol_scoping::build(const sema::context&          ctx,
                           const mod::module&            entry_mod,
                           gsl::span<mod::module* const> deps) -> symbol_scoping {
    PROFILE_FUNCTION();
    symbol_scoping   policy;
    name_count_map_t name_counts;
    str_set_t        weak_names;
    scan_state       st{policy.scopable_defs_, name_counts, weak_names, ctx, entry_mod};

    scan_module(st, entry_mod);
    for (auto* dep : deps) {
        if (dep) { scan_module(st, *dep); }
    }

    for (const auto& [name, count] : name_counts) {
        if (count > 1 && !weak_names.contains(name)) { policy.collided_names_.emplace(name); }
    }
    return policy;
}

auto symbol_scoping::is_scoped(usize owner_table_idx, std::string_view name) const -> bool {
    if (!collided_names_.contains(std::string{name})) { return false; }
    return scopable_defs_.contains(key_of(owner_table_idx, name));
}

auto symbol_scoping::name_for(usize owner_table_idx, std::string_view name) const -> std::string {
    if (!is_scoped(owner_table_idx, name)) { return std::string{name}; }
    return fmt::format("m{}.{}", owner_table_idx, name);
}

} // namespace ghoti::gir
