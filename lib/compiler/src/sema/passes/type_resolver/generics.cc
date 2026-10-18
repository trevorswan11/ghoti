#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <fmt/ranges.h>
#include <gsl/span>
#include <gsl/util>
#include <stdx/assert.hh>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/ast/format.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/statement.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/generic.hh"
#include "compiler/sema/impl_registry.hh"
#include "compiler/sema/instantiation_cache.hh"
#include "compiler/sema/passes/type_resolver.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/sema/unwrap_shape.hh"
#include "compiler/syntax/builtins.hh"
#include "internal.hh"

namespace ghoti::sema {

using namespace resolver_detail;

namespace {

// The node id of the struct/union/enum literal/synthesis a `fn(...): type` body returns, if any.
[[nodiscard]] auto returned_aggregate_node(const ast::AST& ast, const ast::block_stmt& block)
    -> stdx::option<ast::node_id> {
    for (const auto& stmt : block) {
        const auto ret{ast.get_as_opt<ast::return_stmt>(stmt)};
        if (!ret || !ret->expression) { continue; }
        const ast::node_id expr{*ret->expression};
        if (expr.any<ast::struct_expr, ast::union_expr, ast::enum_expr>()) { return expr; }
        if (const auto call{ast.get_as_opt<ast::call_expr>(expr)}) {
            const auto builtin{call->function->get_builtin()};
            if (builtin == syntax::builtin_t::STRUCT || builtin == syntax::builtin_t::UNION ||
                builtin == syntax::builtin_t::ENUM) {
                return expr;
            }
        }
    }
    return stdx::none;
}

[[nodiscard]] auto aggregate_member_list(const ast::AST& ast, ast::node_id agg)
    -> stdx::option<const ast::member_list&> {
    if (const auto s{ast.get_as_opt<ast::struct_expr>(agg)}) { return s->members; }
    if (const auto u{ast.get_as_opt<ast::union_expr>(agg)}) { return u->members; }
    if (const auto e{ast.get_as_opt<ast::enum_expr>(agg)}) { return e->members; }
    return stdx::none;
}

// Records one `context::type_ctor_member_emit` per `const m = fn ...` member of the aggregate
// returned by a `fn(...): type` constructor.
auto register_type_ctor_members(context&         ctx,
                                mod::module_t&   fn_mod,
                                ast::node_id     agg_node,
                                type&            src_agg,
                                type&            clone,
                                std::string_view ctor_mangled,
                                body_type_diff   typing) -> void {
    const auto members{aggregate_member_list(fn_mod.ast, agg_node)};
    if (!members) { return; }

    for (const auto& m : *members) {
        const auto decl{fn_mod.ast.get_as_opt<ast::decl_stmt>(*m)};
        if (!decl || !decl->value) { continue; }
        const auto fn_expr{fn_mod.ast.get_as_opt<ast::function_expr>(*decl->value)};
        if (!fn_expr) { continue; }
        // A generic member is monomorphized per call instead, under its own instance name
        if (const auto fn_type{fn_mod.get_sema_type_opt(*decl->value)}) {
            if (fn_expr->self && ctx.generic_functions.get_opt(*fn_type)) { continue; }
            const auto sig{fn_type->get_data().as_opt<types::function>()};
            if (!fn_expr->self && sig && any_param_generic(sig->params)) { continue; }
        }
        const auto& name{fn_mod.ast.get_as<ast::identifier_expr>(decl->name).name};
        fn_mod.type_ctor_member_emits.emplace_back<type_ctor_member_emit>({
            .owner_clone = &clone,
            .member_decl = *m,
            .gir_name    = fmt::format("{}.{}", ctor_mangled, name),
            .typing_key  = std::string{ctor_mangled},
        });
    }

    // The replay must place `@This()` / `.{ ... }` / `^self` nodes at `clone`, not the shared
    // literal type that later instantiations overwrite.
    for (auto& [_, ty] : typing.node_types) {
        if (ty) { ty.emplace(remap_type(ctx, *ty, src_agg, clone)); }
    }
    for (auto& [_, ty] : typing.explicit_types) {
        if (ty) { ty.emplace(remap_type(ctx, *ty, src_agg, clone)); }
    }

    // `diff_into` only captures types that changed since the snapshot, so pin the `clone`-remapped
    // form of every node type that still reaches `src_agg` into the diff explicitly
    auto& node_types{fn_mod.sema_side_tables.node_types.values};
    for (usize idx{0}; idx < node_types.size(); ++idx) {
        auto& ty{node_types[idx]};
        if (!ty) { continue; }
        auto& remapped{remap_type(ctx, *ty, src_agg, clone)};
        if (&remapped != ty.get()) {
            typing.node_types.insert_or_assign(idx, stdx::option<type&>{remapped});
        }
    }
    auto& explicit_types{fn_mod.sema_side_tables.explicit_types.values};
    for (usize idx{0}; idx < explicit_types.size(); ++idx) {
        auto& ty{explicit_types[idx]};
        if (!ty) { continue; }
        auto& remapped{remap_type(ctx, *ty, src_agg, clone)};
        if (&remapped != ty.get()) {
            typing.explicit_types.insert_or_assign(idx, stdx::option<type&>{remapped});
        }
    }

    if (!typing.empty()) {
        ctx.instantiation_cache.set_body_type_diff(std::string{ctor_mangled}, std::move(typing));
    }
    ctx.generic_functions.set_type_ctor_member_prefix(clone, std::string{ctor_mangled});
}

// The pool entry holding an instantiation's copy of the anonymous aggregate `src`, keyed by
// `disc`, except an `abstract` instance (an impl template) which stands for any instance
[[nodiscard]] auto anonymous_aggregate_slot(context&         ctx,
                                            const type&      src,
                                            std::string_view disc,
                                            bool             abstract = false) -> type& {
    types::key_t key{src.get_kind(), src.get_key().get_mut()};
    key.imprint(disc);
    auto& slot{*ctx.pool[key]};
    if (slot.get_instance_id() != 0) { return slot; }
    if (src.has_symbol_table_idx()) { slot.set_symbol_table_idx(src.get_symbol_table_idx()); }
    if (abstract) { return slot; }
    stdx::hasher id_hash{0};
    id_hash.combine(disc);
    slot.set_instance_id(id_hash.finalize() | 1); // never the "no instance" zero
    ctx.generic_functions.set_clone_disc(slot.get_instance_id(), std::string{disc});
    return slot;
}

// Copies an anonymous aggregate into its per-instantiation pool entry so repeated requests
// share one type. A `provisional` slot only borrowed the literal's shape, so it is refilled
[[nodiscard]] auto clone_anonymous_aggregate(context&         ctx,
                                             type&            src,
                                             std::string_view disc,
                                             bool             provisional = false,
                                             bool             abstract    = false) -> type& {
    auto& fresh{anonymous_aggregate_slot(ctx, src, disc, abstract)};
    if (fresh.is_resolved() && !provisional) { return fresh; }

    // Rebind so distinct instantiations of a `fn(T): type` ctor do not alias each other's shape
    const auto rebind{[&](gsl::span<type*> types) -> gsl::span<type*> {
        auto out{ctx.pool.get_many_unsafe(types.size())};
        for (usize i{0}; i < types.size(); ++i) {
            out[i] = &remap_type(ctx, *types[i], src, fresh);
        }
        return out;
    }};

    src.get_data().visit(
        [](const auto&) { UNREACHABLE("clone_anonymous_aggregate on a non-aggregate type"); },
        [&](const types::struct_t& s) {
            fresh.resolve<types::struct_t>(rebind(s.fields),
                                           s.ast_fields,
                                           rebind(s.members),
                                           s.enclosing,
                                           s.is_c_abi,
                                           s.is_packed,
                                           s.field_alignments);
        },
        [&](const types::union_t& u) {
            fresh.resolve<types::union_t>(rebind(u.fields),
                                          u.ast_fields,
                                          rebind(u.members),
                                          u.enclosing,
                                          u.is_untagged,
                                          u.is_c_abi,
                                          u.is_packed,
                                          u.tag_enum);
        },
        [&](const types::enum_t& e) {
            fresh.resolve<types::enum_t>(
                e.ast_enumerations, e.non_exhaustive, e.underlying, rebind(e.members), e.enclosing);
        });
    return fresh;
}

// A type ctor's literal type and its member types are shared by every instance, which each
// resolve them in place. This is one instance's shape, put back after a nested instance.
struct shared_literal_shape {
    type*                                       literal;
    type::data_t                                data;
    std::vector<std::pair<type*, type::data_t>> members;

    [[nodiscard]] static auto save(type& literal) -> shared_literal_shape {
        shared_literal_shape shape{.literal = &literal, .data = literal.get_data(), .members = {}};
        const auto           save_members{[&](gsl::span<type*> members) {
            for (auto* m : members) {
                if (m && m->is_resolved()) { shape.members.emplace_back(m, m->get_data()); }
            }
        }};
        literal.get_data().visit([](const auto&) {},
                                 [&](const types::struct_t& st) { save_members(st.members); },
                                 [&](const types::union_t& un) { save_members(un.members); },
                                 [&](const types::enum_t& en) { save_members(en.members); });
        return shape;
    }

    auto restore() const -> void {
        literal->resolve<type::data_t>(data);
        for (const auto& [member, member_data] : members) {
            member->resolve<type::data_t>(member_data);
        }
    }
};

// Whether an instance of the ctor owning `literal`, other than `slot`, is mid-instantiation
[[nodiscard]] auto
other_instance_in_progress(const context& ctx, const type& literal, const type& slot) -> bool {
    return std::ranges::any_of(ctx.instantiations_in_progress, [&](const auto& entry) {
        const type* other{entry.second};
        return other && other != &slot && other->get_instance_id() != 0 &&
               other->get_symbol_table_idx_opt() == literal.get_symbol_table_idx_opt();
    });
}

// How diagnostics and `@typeName` spell one instance of a type ctor, e.g. `Box(u8)`
[[nodiscard]] auto
ctor_instance_display_name(const context&                                               ctx,
                           std::string_view                                             ctor,
                           const std::vector<std::pair<std::string, gir::const_value>>& bindings)
    -> std::string {
    const auto show{[&](const gir::const_value& v) -> std::string {
        if (const auto t{v.as_opt<stdx::option<type&>>()}; t && *t) {
            return ctx.type_display_name(**t);
        }
        if (const auto b{v.as_opt<bool>()}) { return *b ? "true" : "false"; }
        if (const auto i{v.as_int_opt()}) { return fmt::format("{}", static_cast<i64>(*i)); }
        return "_";
    }};
    return fmt::format("{}({})",
                       ctor,
                       fmt::join(bindings | std::views::transform([&](const auto& binding) {
                                     return show(binding.second);
                                 }),
                                 ", "));
}

// Replaces characters that are meaningful in a discriminator but undesirable in a symbol name.
[[nodiscard]] auto sanitize_mangled(std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c == '#' || c == '.' || c == ':') {
            out += '_';
        } else {
            out += c;
        }
    }
    return out;
}

// Mangles a type such that it will not conflict with any other monomorphized instance
[[nodiscard]] auto mangle_arg_type(const generic_function_registry& reg, const type& t)
    -> std::string {
    if (const auto meta{t.get_data().as_opt<types::meta_type>()}) {
        return "type_" + mangle_arg_type(reg, meta->instance);
    }
    const auto& data{t.get_data()};
    switch (t.get_kind()) {
    case type_kind::INT:    return type_kind_display_name(t); // `i32`, `u17`, ... (width-distinct)
    case type_kind::STRUCT:
    case type_kind::UNION:
    case type_kind::ENUM:   {
        const auto kind_name{type_kind_display_name(t)};
        if (const auto disc{reg.get_clone_disc(t.get_instance_id())}) {
            return fmt::format("{}${}", kind_name, sanitize_mangled(*disc));
        }
        if (t.has_symbol_table_idx()) {
            return fmt::format("{}{}", kind_name, t.get_symbol_table_idx());
        }
        return std::string{kind_name};
    }
    case type_kind::CLOSURE: {
        const auto& cl{data.as<types::closure_t>()};
        return "closure_" + mangle_arg_type(reg, cl.signature);
    }
    case type_kind::FUNCTION: {
        const auto& fn{data.as<types::function>()};
        return fmt::format("{}_{}__{}",
                           fn.erased ? "fn" : "xfn",
                           fmt::join(fn.params | std::views::transform([&](const auto* p) {
                                         return mangle_arg_type(reg, *p);
                                     }),
                                     "_"),
                           mangle_arg_type(reg, fn.return_type));
    }
    case type_kind::ARRAY: {
        const auto& arr{data.as<types::array>()};
        return fmt::format("array{}{}_{}{}",
                           arr.len,
                           arr.null_terminated ? "z" : "",
                           arr.underlying.is_constant() ? "" : "mut_",
                           mangle_arg_type(reg, arr.underlying));
    }
    case type_kind::SLICE: {
        const auto& sl{data.as<types::slice>()};
        return fmt::format("slice{}_{}{}",
                           sl.null_terminated ? "z" : "",
                           sl.underlying.is_constant() ? "" : "mut_",
                           mangle_arg_type(reg, sl.underlying));
    }
    case type_kind::POINTER: {
        const auto& p{data.as<types::pointer>()};
        return fmt::format("ptr_{}{}",
                           p.underlying.is_constant() ? "" : "mut_",
                           mangle_arg_type(reg, p.underlying));
    }
    case type_kind::REFERENCE: {
        const auto& r{data.as<types::reference>()};
        return fmt::format("ref_{}{}",
                           r.underlying.is_constant() ? "" : "mut_",
                           mangle_arg_type(reg, r.underlying));
    }
    default: return std::string{type_kind_display_name(t)};
    }
}

} // namespace

auto type_resolver::record_type_ctor_member_call(ast::node_id call_id, const ast::call_expr& call)
    -> void {
    stdx::option<std::string_view> member_name;
    stdx::option<type&>            obj_type;
    if (const auto dot{resolving_.ast.get_as_opt<ast::dot_expr>(call.function)}) {
        member_name.emplace(resolving_.ast.get_as<ast::identifier_expr>(dot->member).name);
        obj_type = resolving_.get_sema_type_opt(dot->object);
    } else if (const auto imp{
                   resolving_.ast.get_as_opt<ast::implicit_access_expr>(call.function)}) {
        member_name.emplace(resolving_.ast.get_as<ast::identifier_expr>(imp->member).name);
        obj_type = implicit_type_stack_.peek();
    }
    if (!member_name || !obj_type) { return; }

    type* t{obj_type.get()};
    if (const auto meta{t->get_data().as_opt<types::meta_type>()}) { t = &meta->instance; }

    if (const auto prefix{ctx_.generic_functions.get_type_ctor_member_prefix(*t)}) {
        resolving_.set_generic_call_target(call_id, fmt::format("{}.{}", *prefix, *member_name));
    }
}

auto type_resolver::register_non_generic_type_ctor_members(type&                 result,
                                                           const ast::call_expr& ctor_call)
    -> void {
    const auto k{result.get_kind()};
    if (k != type_kind::STRUCT && k != type_kind::UNION && k != type_kind::ENUM) { return; }
    if (ctx_.generic_functions.get_type_ctor_member_prefix(result)) { return; }

    // Resolve the constructor's declaration and its owning module from `Ctor()` or `mod.Ctor()`.
    mod::module_t*        owner_mod{nullptr};
    stdx::option<symbol&> ctor_sym;
    if (const auto fn_ident{resolving_.ast.get_as_opt<ast::identifier_expr>(ctor_call.function)}) {
        if (!resolving_.root_table_idx) { return; }
        owner_mod = &resolving_;
        ctor_sym  = ctx_.registry.get_from_opt(*resolving_.root_table_idx, fn_ident->name);
    } else if (const auto dot{resolving_.ast.get_as_opt<ast::dot_expr>(ctor_call.function)}) {
        const auto mod_type{resolving_.get_sema_type_opt(dot->object)};
        const auto m_data{mod_type ? mod_type->get_data().as_opt<types::module_t>() : stdx::none};
        if (!m_data || !m_data->imported.root_table_idx) { return; }
        owner_mod = &m_data->imported;
        const auto& inner{resolving_.ast.get_as<ast::identifier_expr>(dot->member)};
        ctor_sym = ctx_.registry.get_from_opt(*owner_mod->root_table_idx, inner.name);
    }
    if (!owner_mod || !ctor_sym) { return; }

    const auto node{ctor_sym->get_data().as_opt<symbols::node_t>()};
    if (!node) { return; }
    const auto decl{owner_mod->ast.get_as_opt<ast::decl_stmt>(*node)};
    if (!decl || !decl->value) { return; }
    const auto fn_expr{owner_mod->ast.get_as_opt<ast::function_expr>(*decl->value)};
    if (!fn_expr) { return; }

    const auto& block{owner_mod->ast.get_as<ast::block_stmt>(*fn_expr->body)};
    const auto  agg_node{returned_aggregate_node(owner_mod->ast, block)};
    if (!agg_node) { return; }

    const auto prefix{fmt::format(
        "tc{}", result.has_symbol_table_idx() ? result.get_symbol_table_idx() : usize{0})};
    register_type_ctor_members(ctx_, *owner_mod, *agg_node, result, result, prefix, {});
}

auto type_resolver::lend_shape_to_pending_instance(usize literal_idx, const type& literal) -> void {
    if (!pending_instance_slot_ || pending_instance_slot_->first != literal_idx) { return; }
    auto& slot{*pending_instance_slot_->second};
    if (!slot.is_resolved()) { slot.resolve<type::data_t>(literal.get_data()); }
}

namespace {

// The symbol a monomorph is emitted under, distinct per argument types, `comptime` values, and
// enclosing type (`Box(u8).apply` vs `Box(i64).apply`)
[[nodiscard]] auto instance_mangled_name(context&                          ctx,
                                         const generic_function_info&      fn_info,
                                         gsl::span<type*>                  concrete_args,
                                         gsl::span<const gir::const_value> comptime_args)
    -> std::string {
    auto name{
        fmt::format("{}__{}",
                    ctx.instantiation_cache.instance_name(
                        fn_info.name.value_or("fn"), reinterpret_cast<uptr>(fn_info.fn_expr.get())),
                    fmt::join(concrete_args | std::views::transform([&](const auto& arg) {
                                  return mangle_arg_type(ctx.generic_functions, *arg);
                              }),
                              "_"))};
    for (const auto& cx : comptime_args) { name += fmt::format("_cx{}", cx.mangle()); }
    if (fn_info.enclosing_type) {
        name +=
            fmt::format("_in_{}", mangle_arg_type(ctx.generic_functions, *fn_info.enclosing_type));
    }
    return name;
}

// Undoes what instantiating left in `fn_mod`'s shared side tables, since the monomorph's own
// typing lives in `typing`
auto restore_shared_typing(mod::module_t&                fn_mod,
                           const body_typing_snapshot&   snap,
                           const body_type_diff&         typing,
                           const body_write_log&         write_log,
                           stdx::option<body_write_log&> outer_log,
                           usize                         literal_idx) -> void {
    // Outside the overlay the literal reads as its template again, not the last instantiation
    if (const auto templ{snap.attributes.find(literal_idx)}; templ != snap.attributes.end()) {
        fn_mod.node_attributes.insert_or_assign(literal_idx, templ->second);
    } else {
        fn_mod.node_attributes.erase(literal_idx);
    }

    auto&      tables{fn_mod.sema_side_tables};
    const auto rollback_poisoned{[](auto& live, const auto& snapshot, const auto& changed) {
        for (const auto& [idx, ty] : changed) {
            if (ty && ty->is_poison() && idx < snapshot.size()) { live[idx] = snapshot[idx]; }
        }
    }};
    rollback_poisoned(tables.node_types.values, snap.nodes, typing.node_types);
    rollback_poisoned(tables.explicit_types.values, snap.types, typing.explicit_types);

    // Nested in another instantiation of this module, this one may have retyped nodes the outer
    // one already typed, so hand those back for the outer diff to record
    if (outer_log) {
        const auto give_back{[](auto&                     live,
                                const auto&               snapshot,
                                const std::vector<usize>& ours,
                                const std::vector<usize>& enclosing) {
            const ankerl::unordered_dense::set<usize> enclosing_idxs(enclosing.begin(),
                                                                     enclosing.end());
            for (const auto idx : ours) {
                if (idx < snapshot.size() && enclosing_idxs.contains(idx)) {
                    live[idx] = snapshot[idx];
                }
            }
        }};
        give_back(tables.node_types.values, snap.nodes, write_log.node_idxs, outer_log->node_idxs);
        give_back(tables.explicit_types.values,
                  snap.types,
                  write_log.explicit_idxs,
                  outer_log->explicit_idxs);
    }
    for (const auto& [idx, target] : typing.call_targets) {
        if (idx < snap.calls.size()) { tables.generic_call_targets.values[idx] = snap.calls[idx]; }
    }
}

// A `fn(...): type` constructor needs every value parameter `comptime`, reporting the first that
// isn't
[[nodiscard]] auto check_ctor_params_comptime(context&                  ctx,
                                              const mod::module_t&      fn_mod,
                                              const ast::function_expr& fn_expr) -> bool {
    for (const auto& param : fn_expr.parameters) {
        if (param.is_comptime) { continue; }
        // An untyped pack (`rest...`) has no annotation, and its elements are values
        const bool untyped_pack{!param.explicit_type};
        const auto pty{untyped_pack ? stdx::none : fn_mod.get_sema_type_opt(*param.explicit_type)};
        if (!untyped_pack && (!pty || pty->get_kind() == type_kind::TYPE)) { continue; }
        std::string_view pn{ast::parameter_name(fn_mod.ast, param)};
        if (pn.empty()) { pn = "_"; }
        ctx.diags.emplace_back(
            fmt::format("a `fn(...): type` constructor cannot take a plain value parameter; mark "
                        "'{}' `comptime` so it is known at instantiation time",
                        pn),
            error::TYPE_MISMATCH,
            fn_mod.ast.location_of(param.name));
        return false;
    }
    return true;
}

// A type ctor instance's `comptime` values and plain `type` arguments, by parameter name
[[nodiscard]] auto ctor_bindings_of(const ast::AST&                   tree,
                                    const ast::function_expr&         fn_expr,
                                    gsl::span<type*>                  concrete_args,
                                    gsl::span<const gir::const_value> comptime_args)
    -> std::vector<std::pair<std::string, gir::const_value>> {
    std::vector<std::pair<std::string, gir::const_value>> bindings;
    for (usize p_idx{0}, cx_i{0}; p_idx < fn_expr.parameters.size(); ++p_idx) {
        const auto& param{fn_expr.parameters[p_idx]};
        const auto  named{param.name.is<ast::identifier_expr>()};
        if (binds_comptime(tree, fn_expr, p_idx, concrete_args)) {
            if (cx_i < comptime_args.size() && named) {
                bindings.emplace_back(
                    std::string{tree.get_as<ast::identifier_expr>(param.name).name},
                    comptime_args[cx_i]);
            }
            ++cx_i;
        } else if (p_idx < concrete_args.size() && named) {
            bindings.emplace_back(std::string{tree.get_as<ast::identifier_expr>(param.name).name},
                                  gir::const_value{denoted_type(*concrete_args[p_idx])});
        }
    }
    return bindings;
}

// The `impl` block on the generic's enclosing type that declares it as a method, if any
[[nodiscard]] auto enclosing_impl_of(context& ctx, const generic_function_info& fn_info)
    -> stdx::option<const impl_record&> {
    if (!fn_info.enclosing_type) { return stdx::none; }
    const auto fn_idx{fn_info.node_id.get_index()};
    for (const auto* r : ctx.impls.records()) {
        if (r->target_type != fn_info.enclosing_type) { continue; }
        const bool is_member{std::ranges::any_of(r->methods, [&](const auto& m) {
            if (m.decl.get_index() == fn_idx) { return true; }
            if (!r->enclosing) { return false; }
            const auto decl{r->enclosing->ast.get_as_opt<ast::decl_stmt>(m.decl)};
            return decl && decl->value && decl->value->get_index() == fn_idx;
        })};
        if (is_member) { return *r; }
    }
    return stdx::none;
}

// A generic whose declaration is `@[deprecated]`, so its monomorphs' bodies stay quiet too
[[nodiscard]] auto declares_deprecated_fn(const mod::module_t& owner, ast::node_id node) -> bool {
    auto decl{owner.ast.get_as_opt<ast::decl_stmt>(node)};
    if (const auto fn{owner.ast.get_as_opt<ast::function_expr>(node)}; fn && fn->declaring_decl) {
        decl = owner.ast.get_as_opt<ast::decl_stmt>(*fn->declaring_decl);
    }
    return decl && decl->attributes && decl->attributes->find(ast::attribute_kind::DEPRECATED);
}

} // namespace

auto type_resolver::binding_restore_guard::retype(mod::module_t& owner, ast::node_id node, type& t)
    -> void {
    const auto prev{owner.sema_side_tables.node_types[node]};
    restores.emplace_back([&owner, node, prev] { owner.sema_side_tables.node_types[node] = prev; });
    owner.set_sema_type(node, t);
}

auto type_resolver::binding_restore_guard::bind_value_symbol(context&         ctx,
                                                             usize            table_idx,
                                                             std::string_view name) -> void {
    const auto sym{ctx.registry.get_from_opt(table_idx, name)};
    if (!sym) { return; }
    const auto prev_kind{sym->get_kind_opt()};
    const auto prev_status{sym->get_status()};
    restores.emplace_back([&ctx, table_idx, name, prev_kind, prev_status] {
        if (auto s{ctx.registry.get_from_opt(table_idx, name)}) {
            if (prev_kind) { s->set_kind(*prev_kind); }
            s->set_status(prev_status);
        }
    });
    sym->set_kind(symbol_kind::VALUE);
    sym->set_status(symbol_status::RESOLVED);
}

// Binds a parameterized impl's params to this instance's type arguments and ctor values
auto type_resolver::bind_impl_params(mod::module_t&         fn_mod,
                                     const impl_record&     impl,
                                     comptime_frame&        frame,
                                     binding_restore_guard& restores) -> void {
    const auto& impl_stmt{fn_mod.ast.get_as<ast::impl_stmt>(impl.site)};
    for (usize i{0}; i < impl_stmt.impl_params.size(); ++i) {
        if (i >= impl.type_arguments.size() || !impl.type_arguments[i]) { continue; }
        const auto  param_name{impl_stmt.impl_params[i].name};
        const auto& pname{fn_mod.ast.get_as<ast::identifier_expr>(param_name).name};
        auto&       concrete_t{denoted_type(*impl.type_arguments[i])};
        frame.insert_or_assign(pname, gir::const_value{concrete_t});
        restores.retype(fn_mod, param_name, concrete_t);
        restores.bind_value_symbol(ctx_, impl.body_scope_idx, pname);
    }
    if (impl.gir_prefix.empty()) { return; }
    if (const auto cx_bindings{ctx_.instantiation_cache.get_type_ctor_bindings(impl.gir_prefix)}) {
        for (const auto& [pname, val] : *cx_bindings) { frame.insert_or_assign(pname, val); }
    }
}

// Binds the enclosing type ctor's parameters for a generic member of one of its instances
auto type_resolver::bind_ctor_member_params(const generic_function_info& fn_info,
                                            comptime_frame&              frame,
                                            binding_restore_guard&       restores) -> void {
    const auto prefix{ctx_.generic_functions.get_type_ctor_member_prefix(*fn_info.enclosing_type)};
    if (!prefix) { return; }
    const auto cx_bindings{ctx_.instantiation_cache.get_type_ctor_bindings(*prefix)};
    if (!cx_bindings) { return; }
    for (const auto& [pname, val] : *cx_bindings) {
        frame.insert_or_assign(pname, val);
        if (!fn_info.enclosing_fn_table_idx) { continue; }
        const auto vt{val.get_type()};
        if (!vt) { continue; }
        const auto table_idx{*fn_info.enclosing_fn_table_idx};
        const auto s{ctx_.registry.get_from_opt(table_idx, pname)};
        if (!s) { continue; }
        const auto p{s->get_data().as_opt<symbols::parameter>()};
        if (!p) { continue; }
        restores.retype(*fn_info.module, ast::node_id{p->name}, *vt);
        restores.bind_value_symbol(ctx_, table_idx, pname);
    }
}

// The type `self` has in a monomorph: its enclosing type behind `self`'s pointer or reference
auto type_resolver::instance_self_type(const generic_function_info& fn_info)
    -> stdx::option<type&> {
    const auto& self{fn_info.fn_expr->self};
    if (!self) { return stdx::none; }
    if (!fn_info.enclosing_type) { return fn_info.module->get_sema_type_opt(self->name); }
    auto&      enc{*fn_info.enclosing_type};
    const auto m{types::mut::from_type_modifier(self->modifier)};
    if (!m) { return enc; }
    return self->modifier.is_ptr() ? ctx_.get_pointer(*m, enc) : ctx_.get_reference(*m, enc);
}

auto type_resolver::instantiate_generic(type&                             callee_type,
                                        const generic_function_info&      fn_info,
                                        gsl::span<type*>                  concrete_args,
                                        gsl::span<const gir::const_value> comptime_args)
    -> stdx::option<generic_instantiation_entry> {
    mod::module_t& fn_mod{*fn_info.module};
    const auto&    fn_expr{*fn_info.fn_expr};
    if (ctx_.generic_instantiation_depth >= context::max_generic_instantiation_depth) {
        ctx_.diags.emplace_back(
            fmt::format("Instantiating '{}' recursed more than {} levels deep; a generic that "
                        "instantiates itself with ever-new arguments never terminates",
                        fn_info.name.value_or("<generic function>"),
                        context::max_generic_instantiation_depth),
            error::COMPTIME_RECURSION_LIMIT_EXCEEDED,
            fn_mod.ast.location_of(fn_info.node_id));
        return stdx::none;
    }
    ++ctx_.generic_instantiation_depth;
    const auto depth_restore{gsl::finally([&] { --ctx_.generic_instantiation_depth; })};
    const auto fn_type{fn_info.fn_type};
    const auto fn_table_idx{fn_type->get_symbol_table_idx()};

    // Computed early so a nested `for`/`while comptime`'s per-iteration typing keys can be scoped
    // to this instantiation before its body is resolved
    auto mangled_name{instance_mangled_name(ctx_, fn_info, concrete_args, comptime_args)};

    // A recursive call with the same arguments names the monomorph its body is still resolving
    if (!building_param_template_) {
        if (const auto it{ctx_.instantiations_in_progress.find(mangled_name)};
            it != ctx_.instantiations_in_progress.end()) {
            if (!it->second) {
                ctx_.diags.emplace_back(
                    fmt::format("'{}' calls itself with the same arguments, so its return type "
                                "must be written out instead of inferred",
                                fn_info.name.value_or("<generic function>")),
                    error::COMPTIME_RECURSION_LIMIT_EXCEEDED,
                    fn_mod.ast.location_of(fn_info.node_id));
                return stdx::none;
            }
            return generic_instantiation_entry{.return_type  = it->second,
                                               .mangled_name = mangled_name};
        }
    }

    // Snapshot the shared side tables so this instantiation's typing is captured as a replayable
    // diff
    ctx_.advance_epoch();
    const body_typing_snapshot snap{fn_mod};
    binding_restore_guard      binding_restores;

    // Bind each `comptime` parameter to its folded value while this instantiation's body is
    // resolved, so `const_eval` folds it there. `comptime_args` is in parameter order.
    comptime_frame binding_frame;
    for (usize p_idx{0}, cx_i{0}; p_idx < fn_expr.parameters.size(); ++p_idx) {
        if (!binds_comptime(fn_mod.ast, fn_expr, p_idx, concrete_args)) { continue; }
        if (cx_i >= comptime_args.size()) { break; }
        if (fn_expr.parameters[p_idx].name.is<ast::identifier_expr>()) {
            const auto& name{
                fn_mod.ast.get_as<ast::identifier_expr>(fn_expr.parameters[p_idx].name).name};
            binding_frame.insert_or_assign(name, comptime_args[cx_i]);
        }
        ++cx_i;
    }

    const auto enclosing_impl{enclosing_impl_of(ctx_, fn_info)};
    if (enclosing_impl && enclosing_impl->from_parameterized) {
        bind_impl_params(fn_mod, *enclosing_impl, binding_frame, binding_restores);
    } else if (!enclosing_impl && fn_info.enclosing_type) {
        bind_ctor_member_params(fn_info, binding_frame, binding_restores);
    }

    // A top-level generic sees only its own bindings, never a caller's same-named ones
    auto                       outer_frames{fn_info.enclosing_fn_table_idx
                                                ? std::vector<comptime_frame>{}
                                                : std::exchange(ctx_.comptime_binding_frames, {})};
    const auto                 frames_restore{gsl::finally([&] {
        if (!fn_info.enclosing_fn_table_idx) {
            ctx_.comptime_binding_frames = std::move(outer_frames);
        }
    })};
    const comptime_frame_guard cfg{ctx_.comptime_binding_frames, std::move(binding_frame)};

    symbol_table_stack inst_stack;
    inst_stack.push(*ctx_.prelude_index);
    if (fn_mod.root_table_idx) { inst_stack.push(*fn_mod.root_table_idx); }
    if (enclosing_impl) {
        inst_stack.push(enclosing_impl->body_scope_idx);
    } else if (fn_info.enclosing_type && fn_info.enclosing_type->has_symbol_table_idx()) {
        if (fn_info.enclosing_fn_table_idx) { inst_stack.push(*fn_info.enclosing_fn_table_idx); }
        inst_stack.push(fn_info.enclosing_type->get_symbol_table_idx());
    }
    inst_stack.push(fn_table_idx);

    const auto self_t{instance_self_type(fn_info)};
    if (self_t) {
        binding_restores.retype(fn_mod, fn_expr.self->name, *self_t);
        if (const auto ident{fn_mod.ast.get_as_opt<ast::identifier_expr>(fn_expr.self->name)}) {
            binding_restores.bind_value_symbol(ctx_, fn_table_idx, ident->name);
        }
    }

    const bool has_pack{!fn_expr.parameters.empty() && fn_expr.parameters.back().is_pack};
    const auto fixed_param_count{fn_expr.parameters.size() - (has_pack ? 1UZ : 0UZ)};
    ASSERT(has_pack ? fixed_param_count <= concrete_args.size()
                    : fixed_param_count == concrete_args.size(),
           "Arity should be validated in resolve_call");
    for (usize p_idx{0}; p_idx < fixed_param_count; ++p_idx) {
        const auto& param{fn_expr.parameters[p_idx]};
        binding_restores.retype(fn_mod, param.name, *concrete_args[p_idx]);
        if (param.name.is<ast::identifier_expr>()) {
            const auto& ident{fn_mod.ast.get_as<ast::identifier_expr>(param.name)};
            binding_restores.bind_value_symbol(ctx_, fn_table_idx, ident.name);
        }
    }

    // Record so the diff below can be built from exactly what was written rather than
    // inferred by comparing against `snap`
    sema::body_write_log write_log;
    const auto           prev_write_log{fn_mod.active_write_log};
    fn_mod.active_write_log = &write_log;
    const auto restore_write_log{
        gsl::finally([&fn_mod, prev_write_log] { fn_mod.active_write_log = prev_write_log; })};

    type_resolver inst_resolver{fn_mod, ctx_, fn_table_idx, std::move(inst_stack)};
    inst_resolver.for_generic_instantiation_ = true;
    inst_resolver.typing_scope_prefix_       = mangled_name;
    inst_resolver.building_param_template_   = building_param_template_;
    inst_resolver.tentative_instantiation_   = tentative_instantiation_;

    // Re-type body-local decls this instantiation reaches even if a prior monomorphization of the
    // same generic already resolved them
    inst_resolver.reresolve_floor_.emplace(fn_table_idx);
    inst_resolver.deprecated_scope_depth_ =
        deprecated_scope_depth_ + (declares_deprecated_fn(fn_mod, fn_info.node_id) ? 1U : 0U);
    inst_resolver.instantiating_testing_fn_ = declares_testing_fn(fn_mod, fn_info.node_id);

    // This freestanding resolver has no enclosing-type context, so @This() needs it restored.
    stdx::option<structural_guard> this_type_guard;
    if (fn_info.enclosing_type) {
        this_type_guard.emplace(inst_resolver.user_type_stack_, *fn_info.enclosing_type);
    }
    // `comptime` parameters are erased from the monomorph's signature. A pack contributes one
    // slot per trailing argument rather than one for the whole `rest...` declaration.
    const auto pack_elem_count{has_pack ? concrete_args.size() - fixed_param_count : 0UZ};
    usize      fixed_rt_count{0};
    for (usize p_idx{0}; p_idx < fixed_param_count; ++p_idx) {
        if (!binds_comptime(fn_mod.ast, fn_expr, p_idx, concrete_args)) { ++fixed_rt_count; }
    }
    auto           inst_param_types{ctx_.pool.get_many_unsafe(fixed_rt_count + pack_elem_count)};
    comptime_frame type_param_frame;
    usize          i{0};
    for (usize p_idx{0}; p_idx < fixed_param_count; ++p_idx) {
        const auto& param{fn_expr.parameters[p_idx]};
        auto*       arg_type{concrete_args[p_idx]};
        // `impl Fn(...)` resolves here, where the enclosing constructor's parameters are bound
        if (const auto bounds{ctx_.param_bounds.find(&fn_expr)};
            bounds != ctx_.param_bounds.end()) {
            for (const auto& bound : bounds->second) {
                if (bound.param_index != p_idx || !bound.callable) { continue; }
                inst_resolver.resolve(*bound.callable);
                auto& sig{*inst_resolver.last_type_.take()};
                if (sig.is_poison()) { return stdx::none; }
                if (const auto why{inst_resolver.callable_mismatch(sig, *arg_type)}) {
                    const auto& pname{fn_mod.ast.get_as<ast::identifier_expr>(param.name).name};
                    ctx_.diags.emplace_back(
                        fmt::format("`{}` does not match `impl Fn` parameter `{}`: {}",
                                    ctx_.type_display_name(*arg_type),
                                    pname,
                                    *why),
                        error::UNSATISFIED_BOUND,
                        p_idx < instantiating_arg_locations_.size()
                            ? instantiating_arg_locations_[p_idx]
                            : fn_mod.ast.location_of(fn_info.node_id));
                    return stdx::none;
                }
            }
        }
        inst_resolver.resolve_param_type(param.explicit_type);
        type* decl_p_type{arg_type}; // erased type data corresponding to nominal signature type
        type* body_p_type{arg_type}; // contextual type meaning in the function body
        if (inst_resolver.last_type_ && !inst_resolver.last_type_->is_poison()) {
            auto& resolved_param_type{inst_resolver.thin_if_comptime(
                param,
                inst_resolver.concrete_array_type(denoted_type(*inst_resolver.last_type_.take())))};
            // `&auto` / `[]mut auto` has no concrete shape until its argument matched it
            if (!is_auto_pattern(resolved_param_type)) {
                decl_p_type = &resolved_param_type;
                if (resolved_param_type.get_kind() != type_kind::TYPE) {
                    body_p_type = &resolved_param_type;
                }
            }

            // An erased `fn(...)` holds the closure as-is. A thin `comptime` slot binds its type
            if (resolved_param_type.get_kind() == type_kind::FUNCTION &&
                arg_type->get_kind() == type_kind::CLOSURE) {
                const auto cl{arg_type->get_data().as_opt<types::closure_t>()};
                if (cl && is_same_fn_signature(resolved_param_type, cl->signature)) {
                    if (!is_erased_fn(resolved_param_type)) {
                        decl_p_type = arg_type;
                        body_p_type = arg_type;
                    }
                } else {
                    const auto& expected{resolved_param_type.get_data().as<types::function>()};
                    ctx_.diags.emplace_back(
                        fmt::format("Closure does not match the parameter's expected call "
                                    "signature: expected {} parameter(s){}, arity/types differ",
                                    expected.params.size(),
                                    expected.is_variadic ? " (variadic)" : ""),
                        error::CLOSURE_SIGNATURE_MISMATCH,
                        fn_mod.ast.location_of(*param.explicit_type));
                    return stdx::none;
                }
            }
            fn_mod.set_sema_type(*param.explicit_type, resolved_param_type);
            const auto p_name{ast::parameter_name(fn_mod.ast, param)};
            if (resolved_param_type.get_kind() == type_kind::TYPE && !p_name.empty()) {
                const auto val{gir::const_value{denoted_type(*body_p_type)}};
                type_param_frame.insert_or_assign(p_name, val);
                if (!ctx_.comptime_binding_frames.empty()) {
                    ctx_.comptime_binding_frames.back().insert_or_assign(p_name, val);
                }
            }
        } else if (param.explicit_type) {
            fn_mod.set_sema_type(*param.explicit_type, *decl_p_type);
        }
        fn_mod.set_sema_type(param.name, *body_p_type);
        if (!binds_comptime(fn_mod.ast, fn_expr, p_idx, concrete_args)) {
            inst_param_types[i++] = decl_p_type;
        }
    }
    // A pack element has no `param.explicit_type` of its own to resolve against
    if (has_pack) {
        const auto&        pack_param{fn_expr.parameters.back()};
        std::vector<type*> elem_types;
        elem_types.reserve(pack_elem_count);
        for (usize p_idx{fixed_param_count}; p_idx < concrete_args.size(); ++p_idx) {
            elem_types.emplace_back(concrete_args[p_idx]);
            inst_param_types[i++] = concrete_args[p_idx];
        }
        if (pack_param.name.is<ast::identifier_expr>()) {
            const auto& pack_ident{fn_mod.ast.get_as<ast::identifier_expr>(pack_param.name)};
            inst_resolver.current_pack_.emplace(
                pack_binding{.name = pack_ident.name, .element_types = std::move(elem_types)});
        }
    }
    const comptime_frame_guard type_param_guard{ctx_.comptime_binding_frames,
                                                std::move(type_param_frame)};
    inst_resolver.resolve(fn_expr.explicit_return_type);
    if (inst_resolver.last_type_->is_poison()) { return stdx::none; }
    // `denoted_type` unwraps a `@TypeOf(param)` return annotation to the type it names, so it
    // isn't mistaken for a `fn(...): type` type constructor.
    auto& return_type{
        inst_resolver.concrete_array_type(denoted_type(*inst_resolver.last_type_.take()))};
    // Taken after the return type since folding the arguments overwrites `last_type_`
    inst_resolver.resolve_fn_literal_attributes(fn_mod.fn_literal_node(fn_info.node_id), fn_expr);
    const auto is_auto_return{return_type.get_kind() == type_kind::AUTO};
    // Only a declared result is known before the body is, which a recursive call needs. An
    // inferred one isn't, so a recursive call is reported rather than instantiated forever
    const bool track_in_progress{!building_param_template_ &&
                                 return_type.get_kind() != type_kind::TYPE &&
                                 ctx_.instantiations_in_progress
                                     .emplace(mangled_name, is_auto_return ? nullptr : &return_type)
                                     .second};
    // A type constructor's instance is its aggregate's pool slot, known before the body is. A
    // member signature naming this same instance (`Pair(i64)` inside `Pair(i64)`) resolves to it.
    const bool abstract_instance{building_param_template_ || is_abstract_instance(concrete_args)};
    bool       track_ctor_in_progress{false};
    stdx::option<shared_literal_shape> outer_instance_shape;
    if (!building_param_template_ && !track_in_progress &&
        return_type.get_kind() == type_kind::TYPE) {
        const auto& body{fn_mod.ast.get_as<ast::block_stmt>(*fn_expr.body)};
        const auto  agg_node{returned_aggregate_node(fn_mod.ast, body)};
        const auto  literal{
            agg_node && agg_node->any<ast::struct_expr, ast::union_expr, ast::enum_expr>()
                ? fn_mod.get_sema_type_opt(*agg_node)
                : stdx::none};
        if (literal && !literal->is_poison()) {
            auto& slot{
                anonymous_aggregate_slot(ctx_,
                                         *literal,
                                         fmt::format("{}#{}", mangled_name, agg_node->get_index()),
                                         abstract_instance)};
            // An outer instance of this ctor still resolving its members gets back the shape
            // this one is about to write over
            if (literal->is_resolved() && other_instance_in_progress(ctx_, *literal, slot)) {
                outer_instance_shape.emplace(shared_literal_shape::save(*literal));
            }
            track_ctor_in_progress =
                ctx_.instantiations_in_progress.emplace(mangled_name, &slot).second;
            if (track_ctor_in_progress && !slot.is_resolved()) {
                inst_resolver.pending_instance_slot_.emplace(agg_node->get_index(), &slot);
                // Member calls on the instance target its monomorphized members, as they will
                // once it is built
                ctx_.generic_functions.set_type_ctor_member_prefix(slot, mangled_name);
            }
        }
    }
    const auto in_progress_restore{gsl::finally([&] {
        if (track_in_progress || track_ctor_in_progress) {
            ctx_.instantiations_in_progress.erase(mangled_name);
        }
        if (outer_instance_shape) { outer_instance_shape->restore(); }
    })};
    inst_resolver.return_trackers_.emplace_back(return_tracker{
        .return_types   = {},
        .is_auto_return = is_auto_return,
        .expected_type  = is_auto_return ? stdx::none : stdx::option<type&>{return_type},
    });

    const auto  diags_before{ctx_.diags.size()};
    const auto& block{fn_mod.ast.get_as<ast::block_stmt>(*fn_expr.body)};

    bool resolved_poison{false};
    {
        PROFILE_SCOPE("instantiate_generic: resolve body");
        const active_block_guard guard{inst_resolver.active_blocks_, block};
        for (usize idx{0}; idx < block.statements.size(); ++idx) {
            inst_resolver.active_blocks_.back().current_stmt_idx = idx;
            inst_resolver.resolve(block.statements[idx]);
            if (inst_resolver.last_type_->is_poison()) { resolved_poison = true; }
            if (inst_resolver.folded_if_exits(block.statements[idx])) { break; }
        }
    }
    if (ctx_.diags.size() > diags_before || resolved_poison) {
        // Attribute diags here to `fn_mod` so they print against the defining file
        if (ctx_.diags.size() > diags_before && &fn_mod != &resolving_) {
            if (!tentative_instantiation_ && !fn_mod.diagnostics.is<sema::diagnostics>()) {
                fn_mod.error_out(ctx_.diags.split_off(diags_before),
                                 mod::module_state::POISONED_TYPE_RESOLVED);
            } else {
                // `fn_mod` already holds its own list, so the diags stay here pointing into it
                ctx_.diags.set_origin_from(diags_before, fn_mod);
            }
        }
        return stdx::none;
    }

    // Diff the side tables against the snapshot (folding any `comptime`-sized `[n]T` first): all
    // this instantiation's body/signature typing, to be replayed at emit time.
    body_type_diff typing;
    snap.diff_into(ctx_, fn_mod, typing, &write_log);
    // `self` was bound before the write log started, so the emitter needs its type spelled out
    if (self_t) { typing.node_types.insert_or_assign(fn_expr.self->name.get_index(), *self_t); }
    restore_shared_typing(fn_mod,
                          snap,
                          typing,
                          write_log,
                          prev_write_log,
                          fn_mod.fn_literal_node(fn_info.node_id).get_index());
    ctx_.advance_epoch();

    auto tracker{std::move(inst_resolver.return_trackers_.back())};
    inst_resolver.return_trackers_.pop_back();

    const auto ret_kind{return_type.get_kind()};
    if (!is_auto_return && !tracker.has_returns() &&
        (ret_kind == type_kind::COMPTIME_INT || ret_kind == type_kind::COMPTIME_FLOAT ||
         ret_kind == type_kind::TYPE)) {
        ctx_.diags.emplace_back(fmt::format("Function returning '{}' never returns a value",
                                            ctx_.type_display_name(return_type)),
                                error::RETURN_TYPE_MISMATCH,
                                fn_mod.ast.location_of(fn_info.node_id));
        return stdx::none;
    }

    // A `fn(...): type` generic is a type constructor: every parameter is a `type` or a
    // `comptime` value (erased from `inst_param_types`), and the body returns the type it builds.
    const bool all_params_are_types{std::ranges::all_of(
        inst_param_types, [](const type* p) { return p->get_kind() == type_kind::TYPE; })};
    const bool is_type_ctor{!is_auto_return && return_type.get_kind() == type_kind::TYPE &&
                            tracker.has_returns() && all_params_are_types};

    // Force comptime param if constructing a type (inspo from zig)
    if (!is_type_ctor && !is_auto_return && return_type.get_kind() == type_kind::TYPE &&
        tracker.has_returns() && !all_params_are_types &&
        !check_ctor_params_comptime(ctx_, fn_mod, fn_expr)) {
        return stdx::none;
    }
    type* deduced_return_type{(is_auto_return || is_type_ctor) ? &tracker.deduced_return_type(ctx_)
                                                               : &return_type};

    // Copy into a per-instantiation type so `T(i32)` and `T(u8)` don't alias each other
    if (is_type_ctor) {
        deduced_return_type = &denoted_type(*deduced_return_type);
        const auto k{deduced_return_type->get_kind()};
        if (k == type_kind::STRUCT || k == type_kind::UNION || k == type_kind::ENUM) {
            if (const auto agg_node{returned_aggregate_node(fn_mod.ast, block)}) {
                auto& src_agg{*deduced_return_type};
                auto& clone{clone_anonymous_aggregate(
                    ctx_,
                    src_agg,
                    fmt::format("{}#{}", mangled_name, agg_node->get_index()),
                    inst_resolver.pending_instance_slot_.has_value(),
                    abstract_instance)};
                // Call an abstract instantiation built purely to compute a parameterized impl's
                // signatures
                if (!building_param_template_) {
                    register_type_ctor_members(
                        ctx_, fn_mod, *agg_node, src_agg, clone, mangled_name, std::move(typing));
                }

                // Hand this instantiation's `comptime` parameter values, and any plain `type`
                // parameters to the member emit
                auto ctor_bindings{
                    ctor_bindings_of(fn_mod.ast, fn_expr, concrete_args, comptime_args)};
                deduced_return_type = &clone;
                if (ctor_bindings.size() == fn_expr.parameters.size()) {
                    ctx_.user_type_names.try_emplace(
                        &clone,
                        fn_mod.ast.intern(ctor_instance_display_name(
                            ctx_, fn_info.name.value_or("fn"), ctor_bindings)));
                }

                // Expand any parameterized `impl(P) [I for] Ctor(P)` onto this instantiation
                // (before `ctor_bindings` is moved -- the impl needs the ctor's folded values).
                if (!building_param_template_ && !ctx_.impls.param_records().empty()) {
                    instantiate_impls_for(clone,
                                          fn_mod,
                                          fn_info.node_id,
                                          concrete_args,
                                          ctor_bindings,
                                          fn_expr,
                                          mangled_name);
                }

                if (!ctor_bindings.empty()) {
                    ctx_.instantiation_cache.set_type_ctor_bindings(mangled_name,
                                                                    std::move(ctor_bindings));
                }
            }
        }
    }

    // A type constructor has no runtime body
    if (!is_type_ctor) {
        // Hand the folded values to the emitter out-of-band
        if (!comptime_args.empty()) {
            ctx_.instantiation_cache.set_comptime_args(
                mangled_name,
                std::vector<gir::const_value>(comptime_args.begin(), comptime_args.end()));
        }
        std::vector<bool> implicit_comptime(fn_expr.parameters.size(), false);
        for (usize p_idx{0}; p_idx < fixed_param_count; ++p_idx) {
            implicit_comptime[p_idx] =
                binds_implicit_comptime(fn_mod.ast, fn_expr, p_idx, concrete_args);
        }
        if (std::ranges::contains(implicit_comptime, true)) {
            ctx_.instantiation_cache.set_implicit_comptime(mangled_name,
                                                           std::move(implicit_comptime));
        }
        if (!typing.empty()) {
            ctx_.instantiation_cache.set_body_type_diff(mangled_name, std::move(typing));
        }

        generic_instantiation_request req{
            .generic_fn_type = &callee_type,
            .arg_types       = inst_param_types,
            .return_type     = deduced_return_type,
            .mangled_name    = mangled_name,
            .fn_node_id      = fn_info.node_id,
            .module          = &fn_mod,
        };

        fn_mod.generic_instantiations.emplace_back(req);
        if (&resolving_ != &fn_mod) { resolving_.generic_instantiations.emplace_back(req); }
    }

    // A `fn(...): type` constructor that hands back an existing non-aggregate type by name,
    // is itself a value denoting that type and must be wrapped back into a `type`-kind value
    const auto dk{deduced_return_type->get_kind()};
    if (is_type_ctor && dk != type_kind::TYPE && dk != type_kind::STRUCT &&
        dk != type_kind::UNION && dk != type_kind::ENUM) {
        auto& wrapped{*ctx_.pool[{type_kind::TYPE, types::mut::CONSTANT, *deduced_return_type}]};
        wrapped.resolve_if<types::meta_type>(*deduced_return_type);
        deduced_return_type = &wrapped;
    }

    return generic_instantiation_entry{
        .return_type  = deduced_return_type,
        .mangled_name = std::move(mangled_name),
    };
}

} // namespace ghoti::sema
