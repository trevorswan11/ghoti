#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <gsl/span>
#include <gsl/util>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/format.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/statement.hh"
#include "compiler/gir/const_eval.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/sema/alias_chain.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/generic.hh"
#include "compiler/sema/impl_registry.hh"
#include "compiler/sema/instantiation_cache.hh"
#include "compiler/sema/passes/type_resolver.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/sema/unwrap_shape.hh"
#include "internal.hh"
#include "support/diagnostic.hh"

namespace ghoti::sema {

using namespace resolver_detail;

namespace {

// The module a user-defined type was declared in, or null for a builtin / structural type.
[[nodiscard]] auto declaring_module_of(const type& t) -> stdx::option<const mod::module_t&> {
    const auto& d{t.get_data()};
    if (const auto s{d.as_opt<types::struct_t>()}) { return s->enclosing; }
    if (const auto u{d.as_opt<types::union_t>()}) { return u->enclosing; }
    if (const auto e{d.as_opt<types::enum_t>()}) { return e->enclosing; }
    if (const auto i{d.as_opt<types::interface_t>()}) { return i->enclosing; }
    return stdx::none;
}

// An impl `self` must borrow at least as strongly as the requirement.
[[nodiscard]] auto self_binding_compatible(const type& required, const type& provided) -> bool {
    const bool req_ptr{required.get_data().is<types::pointer>()};
    const bool req_ref{required.get_data().is<types::reference>()};
    const bool got_ptr{provided.get_data().is<types::pointer>()};
    const bool got_ref{provided.get_data().is<types::reference>()};
    // A `mut?` method serves both mutabilities, so only a `mut?` method can implement one
    if (required.is_poly() != provided.is_poly()) { return false; }

    // `&mut`/`^mut` requirement needs a mutable impl. Pointer vs reference must agree. By-value
    // needs by-value.
    if (req_ptr) { return got_ptr && (required.is_constant() || !provided.is_constant()); }
    if (req_ref) { return got_ref && (required.is_constant() || !provided.is_constant()); }
    return !got_ptr && !got_ref;
}

} // namespace

// Looks `name` up among `target`'s `impl` methods: none when hidden or absent, the fn type for
// exactly one, and an `AMBIGUOUS_...` error for several
auto type_resolver::register_impl_param_bounds(const ast::function_expr& fn) -> void {
    if (fn.impl_bounds.empty()) { return; }
    std::vector<param_bound> entries;
    for (const auto& b : fn.impl_bounds) {
        // `impl Fn(...)`: a signature for the argument to match
        if (b.interfaces.size() == 1 &&
            b.interfaces.front().get_kind() == ast::explicit_type_kind::FUNCTION) {
            entries.emplace_back<param_bound>({
                .param_index = b.param_index,
                .interfaces  = {},
                .callable    = b.interfaces.front(),
            });
            continue;
        }
        std::vector<const type*> ifaces;
        for (const auto tid : b.interfaces) {
            resolve(tid);
            auto& t{denoted_type(*last_type_.take())};
            if (t.get_kind() == type_kind::INTERFACE) {
                ifaces.emplace_back(&t);
            } else if (!t.is_poison()) {
                ctx_.diags.emplace_back(
                    fmt::format("an `impl` parameter bound must name an interface; found `{}`",
                                ctx_.type_display_name(t)),
                    error::TYPE_MISMATCH,
                    resolving_.ast.location_of(tid));
            }
        }

        // An intersection bound whose interfaces share an associated-item name is rejected
        if (ifaces.size() > 1) {
            ankerl::unordered_dense::map<std::string_view, usize> assoc_counts;
            for (const auto* iface : ifaces) {
                if (const auto it{iface->get_data().as_opt<types::interface_t>()}) {
                    for (const auto n : it->assoc_type_names) { ++assoc_counts[n]; }
                    for (const auto n : it->assoc_const_names) { ++assoc_counts[n]; }
                }
            }

            for (const auto& [aname, count] : assoc_counts) {
                if (count > 1) {
                    ctx_.diags.emplace_back(
                        fmt::format("interfaces in this `impl (...)` bound both declare an "
                                    "associated item `{}`; split the parameter or use a "
                                    "sub-interface",
                                    aname),
                        error::CONFLICTING_ASSOC,
                        resolving_.ast.location_of(b.interfaces.front()));
                }
            }
        }

        if (!ifaces.empty()) {
            entries.emplace_back<param_bound>({
                .param_index = b.param_index,
                .interfaces  = std::move(ifaces),
                .callable    = stdx::none,
            });
        }
    }
    if (!entries.empty()) { ctx_.param_bounds.insert_or_assign(&fn, std::move(entries)); }
}

auto type_resolver::resolve_impl_method_access(const type&      target,
                                               std::string_view name,
                                               source_location  loc)
    -> stdx::option<stdx::result<gsl::not_null<type*>, diagnostic>> {
    const auto                              candidates{ctx_.impls.methods_of(target)};
    std::vector<const impl_record::method*> visible;
    bool                                    hidden_by_seal{false};
    for (const auto& em : candidates) {
        if (em.method->name != name || !em.method->fn_type) { continue; }
        // A non-`pub` trait-impl method is sealed to the interface's declaring module.
        if (em.record->interface_type && !em.method->is_pub) {
            if (declaring_module_of(*em.record->interface_type) != &resolving_) {
                hidden_by_seal = true;
                continue;
            }
        }
        visible.emplace_back(em.method.get());
    }

    if (visible.size() == 1) {
        for (const auto& em : candidates) {
            if (em.method == visible.front()) {
                if (em.record->from_parameterized) {
                    // A parameterized-impl method is emitted under a per-instantiation symbol
                    pending_param_impl_target_.emplace(
                        fmt::format("{}.{}", em.record->gir_prefix, name));
                } else {
                    pending_impl_method_owner_.emplace(em.record->body_scope_idx);
                }
                break;
            }
        }
        return stdx::result<gsl::not_null<type*>, diagnostic>{
            gsl::not_null<type*>{visible.front()->fn_type.get()}};
    }

    if (visible.size() > 1) {
        return stdx::result<gsl::not_null<type*>, diagnostic>{stdx::err{diagnostic{
            fmt::format("call to `{}` is ambiguous: it is provided by more than one `impl`", name),
            error::AMBIGUOUS_METHOD,
            loc}}};
    }

    if (hidden_by_seal) {
        return stdx::result<gsl::not_null<type*>, diagnostic>{stdx::err{diagnostic{
            fmt::format("`{}` is a sealed interface method and is not callable from this module",
                        name),
            error::SEALED_METHOD,
            loc}}};
    }
    return stdx::none;
}

// Resolves an `impl` target/interface reference and, if it names an as-yet-unresolved aggregate,
// forces that aggregate's declaration to resolve so the impl registry can key on final identity.
auto type_resolver::resolve_impl_type_ref(ast::explicit_type_id ref) -> type& {
    resolve(ref);
    auto&      t{denoted_type(*last_type_.take())};
    const auto k{t.get_kind()};
    if (!is_aggregate(k) || t.is_resolved()) { return t; }

    // A forward reference to a not-yet-resolved aggregate: drive its decl to completion.
    if (const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(ref)}) {
        if (const auto lookup{ctx_.registry.lookup(table_stack_, ident->name)}) {
            if (const auto node{lookup->get_data().as_opt<symbols::node_t>()}) {
                if (const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*node)};
                    decl && decl->value) {
                    // `resolve(ref)` above resolves the ident, which can hand back a distinct
                    // unresolved forward twin even after the underlying decl is fully typed
                    const auto typed{resolving_.get_sema_type_opt(*decl->value)};
                    if (typed && (typed->is_resolved() || typed->is_poison())) { return *typed; }
                    if (lookup->get_status() != symbol_status::RESOLVING) {
                        resolve(*decl->value);
                        last_type_.reset();
                        if (const auto now{resolving_.get_sema_type_opt(*decl->value)};
                            now && (now->is_resolved() || now->is_poison())) {
                            return *now;
                        }
                    }
                }
            }
        }
    }
    return t;
}

auto type_resolver::pre_register_impls() -> void {
    PROFILE_FUNCTION();
    for (const auto root : resolving_.ast) {
        const auto impl{resolving_.ast.get_as_opt<ast::impl_stmt>(root)};
        if (!impl) { continue; }
        if (!impl->impl_params.empty()) {
            register_parameterized_impl(root, *impl);
            continue;
        }

        auto&       impl_type{resolving_.get_sema_type(root)};
        const scope s{table_stack_, impl_type.get_symbol_table_idx(), table_idx_};

        stdx::option<type&> interface_type;
        if (impl->interface_type) {
            auto& it{resolve_impl_type_ref(*impl->interface_type)};
            if (it.is_poison()) { continue; }
            interface_type.emplace(it);
        }
        auto& target{resolve_impl_type_ref(impl->target_type)};
        if (target.is_poison()) { continue; }

        // A trait impl is legal in the module that declares `I` or `T`
        stdx::option<mod::module_t&> here{resolving_};
        const auto iface_mod{interface_type ? declaring_module_of(*interface_type) : stdx::none};
        const auto target_mod{declaring_module_of(target)};
        if (interface_type) {
            if (iface_mod != here && target_mod != here) {
                ctx_.diags.emplace_back(
                    "`impl I for T` is only allowed in the module that declares `I` or `T`",
                    error::ORPHAN_IMPL,
                    resolving_.ast.location_of(root));
                continue;
            }
        } else if (target_mod != here) {
            ctx_.diags.emplace_back(
                "an inherent `impl T` requires `T` to be declared in this module",
                error::ORPHAN_IMPL,
                resolving_.ast.location_of(root));
            continue;
        }

        impl_record rec{
            .interface_type = interface_type,
            .target_type    = target,
            .site           = root,
            .enclosing      = here,
            .body_scope_idx = impl_type.get_symbol_table_idx(),
        };
        for (const auto& member : impl->members) {
            const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*member)};
            if (!decl || !decl->value || !decl->value->is<ast::function_expr>()) { continue; }
            rec.methods.emplace_back<impl_record::method>({
                .name    = resolving_.ast.get_as<ast::identifier_expr>(*decl->name).name,
                .decl    = *member,
                .fn_type = stdx::none,
                .is_pub  = decl->has_modifier(ast::decl_modifiers::PUBLIC),
            });
        }

        // An inherent `impl T` member may not shadow a native member of `T` or a member
        // already added by another inherent `impl T` block.
        if (!interface_type && target.has_symbol_table_idx()) {
            const auto& native{ctx_.registry.get(target.get_symbol_table_idx())};
            for (const auto& m : rec.methods) {
                bool clashes{native.has(m.name)};
                for (const auto* other : ctx_.impls.records()) {
                    if (other->interface_type || !other->target_type ||
                        other->target_type != &target) {
                        continue;
                    }
                    clashes = clashes || other->find_method(m.name).has_value();
                }

                if (clashes) {
                    ctx_.diags.emplace_back(
                        fmt::format("`{}` is already a member of `{}`; an inherent `impl` may not "
                                    "redefine it",
                                    m.name,
                                    ctx_.type_display_name(target)),
                        error::DUPLICATE_MEMBER,
                        resolving_.ast.location_of(m.decl));
                }
            }
        }

        auto recorded{ctx_.impls.record(std::move(rec))};
        if (!recorded) {
            const auto prior_loc{resolving_.ast.location_of(recorded.error())};
            ctx_.diags.emplace_back(
                fmt::format("duplicate `impl` for this type; previous impl here: {}", prior_loc),
                error::DUPLICATE_IMPL,
                resolving_.ast.location_of(root));
        }
    }
}

namespace {

// The bare identifier naming a `call_expr` argument, if it is one (`Ctor(T)` → "T" at slot 0).
[[nodiscard]] auto ctor_arg_ident(const mod::module_t& mod, const ast::call_expr::argument& arg)
    -> stdx::option<std::string_view> {
    return arg.visit(
        [&](ast::expr_handle h) -> stdx::option<std::string_view> {
            if (const auto id{mod.ast.get_as_opt<ast::identifier_expr>(*h)}) { return id->name; }
            return stdx::none;
        },
        [&](ast::explicit_type_id t) -> stdx::option<std::string_view> {
            if (const auto id{mod.ast.get_as_opt<ast::identifier_expr>(t)}) { return id->name; }
            return stdx::none;
        });
}

} // namespace

auto type_resolver::param_impl_sentinel(usize disc) -> type& {
    auto& s{*ctx_.pool[{type_kind::TYPE,
                        types::mut::CONSTANT,
                        std::string_view{"pimpl.sentinel"},
                        static_cast<u64>(disc)}]};
    if (!s.is_resolved()) { s.resolve<types::builtin_type>(); }
    return s;
}

// A distinct opaque `type` standing in for one interface associated type inside its method
// signatures, keyed on the assoc's name-node index so `&dyn I(Assoc = T)` can substitute `T`.
auto type_resolver::assoc_type_placeholder(usize disc) -> type& {
    auto& s{*ctx_.pool[{type_kind::TYPE,
                        types::mut::CONSTANT,
                        std::string_view{"iface.assoc"},
                        static_cast<u64>(disc)}]};
    if (!s.is_resolved()) { s.resolve<types::builtin_type>(); }
    return s;
}

namespace {

using decl_value_ref = std::pair<const mod::module_t*, ast::node_id>;

// The module `name` imports in `owner`, read from the import itself so it needs no resolving
[[nodiscard]] auto imported_module(const context&       ctx,
                                   const mod::module_t& owner,
                                   std::string_view name) -> stdx::option<const mod::module_t&> {
    if (!owner.root_table_idx) { return stdx::none; }
    auto sym{ctx.registry.get_from_opt(*owner.root_table_idx, name)};
    if (!sym && ctx.prelude_index) { sym = ctx.registry.get_from_opt(*ctx.prelude_index, name); }
    if (!sym || sym->get_kind_opt() != symbol_kind::MODULE) { return stdx::none; }

    stdx::option<type&> module_type;
    if (const auto builtin{sym->get_data().as_opt<symbols::builtin>()}) {
        module_type.emplace(builtin->get_type());
    } else if (const auto node{sym->get_data().as_opt<symbols::node_t>()}) {
        module_type = owner.get_sema_type_opt(*node);
    }
    const auto data{module_type ? module_type->get_data().as_opt<types::module_t>() : stdx::none};
    if (!data) { return stdx::none; }
    return data->imported;
}

// The value of the declaration an alias expression (`Name` or `module.Name`) in `owner` names
[[nodiscard]] auto aliased_decl_value(const context&       ctx,
                                      const mod::module_t& owner,
                                      ast::node_id         alias) -> stdx::option<decl_value_ref> {
    const auto value_of{
        [&](const mod::module_t& mod, std::string_view name) -> stdx::option<decl_value_ref> {
            if (!mod.root_table_idx) { return stdx::none; }
            const auto sym{ctx.registry.get_from_opt(*mod.root_table_idx, name)};
            const auto node{sym ? sym->get_data().as_opt<symbols::node_t>() : stdx::none};
            const auto decl{node ? mod.ast.get_as_opt<ast::decl_stmt>(*node) : stdx::none};
            if (!decl || !decl->value) { return stdx::none; }
            return decl_value_ref{&mod, *decl->value};
        }};

    if (const auto id{owner.ast.get_as_opt<ast::identifier_expr>(alias)}) {
        return value_of(owner, id->name);
    }
    if (const auto dot{owner.ast.get_as_opt<ast::dot_expr>(alias)}) {
        const auto object{owner.ast.get_as_opt<ast::identifier_expr>(dot->object)};
        const auto imported{object ? imported_module(ctx, owner, object->name) : stdx::none};
        if (!imported) { return stdx::none; }
        return value_of(*imported, owner.ast.get_as<ast::identifier_expr>(dot->member).name);
    }
    return stdx::none;
}

} // namespace

auto type_resolver::register_parameterized_impl(ast::node_id root, const ast::impl_stmt& impl)
    -> void {
    PROFILE_FUNCTION();
    auto&       impl_type{resolving_.get_sema_type(root)};
    const scope s{table_stack_, impl_type.get_symbol_table_idx(), table_idx_};

    // Peel `Ctor(<impl params>)` down to `Ctor` and note which arg slot each impl param fills.
    const auto tgt_call{resolving_.ast.get_as_opt<ast::call_expr>(impl.target_type)};
    std::vector<stdx::option<std::string_view>> ctor_arg_names;
    if (tgt_call) {
        for (const auto& a : tgt_call->arguments) {
            ctor_arg_names.emplace_back(ctor_arg_ident(resolving_, a));
        }
    }

    // Resolve the base ctor identifier to its generic `function_expr` node + declaring module.
    stdx::option<const mod::module_t&> base_mod;
    stdx::option<ast::node_id>         base_ctor_fn;
    if (tgt_call) {
        if (const auto id{resolving_.ast.get_as_opt<ast::identifier_expr>(*tgt_call->function)}) {
            if (const auto sym{ctx_.registry.lookup(table_stack_, id->name)}) {
                if (const auto n{sym->get_data().as_opt<symbols::node_t>()}) {
                    if (const auto d{resolving_.ast.get_as_opt<ast::decl_stmt>(*n)};
                        d && d->value) {
                        base_mod.emplace(resolving_);
                        base_ctor_fn.emplace(*d->value);
                    }
                }
            }
        } else if (const auto dot{resolving_.ast.get_as_opt<ast::dot_expr>(*tgt_call->function)}) {
            resolve(dot->object); // the module alias is not otherwise typed this early
            if (const auto mod_type{resolving_.get_sema_type_opt(dot->object)}) {
                if (const auto md{mod_type->get_data().as_opt<types::module_t>()};
                    md && md->imported.root_table_idx) {
                    const auto& inner{resolving_.ast.get_as<ast::identifier_expr>(dot->member)};
                    if (const auto sym{
                            ctx_.registry.get_from_opt(*md->imported.root_table_idx, inner.name)}) {
                        if (const auto n{sym->get_data().as_opt<symbols::node_t>()}) {
                            if (const auto d{md->imported.ast.get_as_opt<ast::decl_stmt>(*n)};
                                d && d->value) {
                                base_mod.emplace(md->imported);
                                base_ctor_fn.emplace(*d->value);
                            }
                        }
                    }
                }
            }
        }
    }
    // `const Ctor = other.Ctor;` names the same ctor, so anchor at the `fn` it aliases
    alias_chain chain;
    while (base_mod && base_ctor_fn && chain.step(*base_mod, *base_ctor_fn)) {
        const auto& owner{*base_mod};
        if (owner.ast.get_as_opt<ast::function_expr>(*base_ctor_fn)) { break; }
        const auto aliased{aliased_decl_value(ctx_, owner, *base_ctor_fn)};
        if (!aliased) {
            base_ctor_fn.reset();
            break;
        }
        base_mod.emplace(*aliased->first);
        base_ctor_fn.emplace(aliased->second);
    }
    if (!base_mod || !base_ctor_fn) { return; } // cannot anchor: skip quietly

    stdx::option<type&> iface_type;
    if (impl.interface_type) {
        auto& it{resolve_impl_type_ref(*impl.interface_type)};
        if (it.is_poison()) { return; }
        iface_type.emplace(it);
    }

    // The base ctor OR the interface must be declared in this module.
    const auto iface_mod{iface_type ? declaring_module_of(*iface_type) : stdx::none};
    if (base_mod != &resolving_ && iface_mod != &resolving_) {
        ctx_.diags.emplace_back(
            "a parameterized `impl` must be anchored in this module: its base type constructor "
            "or its interface must be declared here",
            error::ORPHAN_IMPL,
            resolving_.ast.location_of(root));
        return;
    }

    std::vector<stdx::opt_size> mapping;
    mapping.reserve(impl.impl_params.size());
    for (const auto& p : impl.impl_params) {
        stdx::opt_size slot;
        if (const auto pn{resolving_.ast.get_as_opt<ast::identifier_expr>(p.name)}) {
            for (usize i{0}; i < ctor_arg_names.size(); ++i) {
                if (ctor_arg_names[i] && *ctor_arg_names[i] == pn->name) {
                    slot.emplace(i);
                    break;
                }
            }
        }
        mapping.emplace_back(slot);
    }

    stdx::option<mod::module_t&> here{resolving_};
    ctx_.impls.record_parameterized(parameterized_impl{
        .site              = root,
        .interface_type    = iface_type,
        .base_ctor_fn      = *base_ctor_fn,
        .base_mod          = base_mod.get(),
        .enclosing         = here,
        .body_scope_idx    = impl_type.get_symbol_table_idx(),
        .param_to_ctor_arg = std::move(mapping),
    });
}

auto type_resolver::instantiate_impls_for(
    type&                                                     concrete,
    const mod::module_t&                                      base_mod,
    ast::node_id                                              base_ctor_fn,
    gsl::span<type* const>                                    ctor_args,
    gsl::span<const std::pair<std::string, gir::const_value>> ctor_cx,
    const ast::function_expr&                                 base_fn,
    std::string_view                                          ctor_mangled) -> void {
    PROFILE_FUNCTION();
    for (auto* pimpl : ctx_.impls.param_records()) {
        // Node indices are per module, so the ctor's module has to match too
        if (pimpl->base_mod.get() != &base_mod ||
            pimpl->base_ctor_fn.get_index() != base_ctor_fn.get_index() ||
            pimpl->base_ctor_fn.get_kind() != base_ctor_fn.get_kind()) {
            continue;
        }
        if (!pimpl->enclosing) { continue; }
        auto&      impl_mod{*pimpl->enclosing};
        const auto impl_stmt{impl_mod.ast.get_as_opt<ast::impl_stmt>(pimpl->site)};
        if (!impl_stmt) { continue; }

        // Bind each impl param: a type param -> the concrete ctor argument type
        std::vector<type*> type_bounds(pimpl->param_to_ctor_arg.size(), nullptr);
        std::vector<std::pair<std::string, gir::const_value>> cx_bindings;
        bool                                                  ok{true};
        bool                                                  is_abstract{false};
        for (usize i{0}; i < pimpl->param_to_ctor_arg.size(); ++i) {
            const auto slot{pimpl->param_to_ctor_arg[i]};
            if (!slot || *slot >= ctor_args.size() || *slot >= base_fn.parameters.size()) {
                ok = false;
                break;
            }
            if (i < impl_stmt->impl_params.size() && impl_stmt->impl_params[i].is_comptime) {
                std::string_view cx_name{};
                if (const auto ident{base_mod.ast.get_as_opt<ast::identifier_expr>(
                        base_fn.parameters[*slot].name)}) {
                    cx_name = ident->name;
                }
                const auto it{std::ranges::find(
                    ctor_cx, cx_name, [](const auto& p) { return std::string_view{p.first}; })};
                if (it == ctor_cx.end()) {
                    ok = false;
                    break;
                }

                cx_bindings.emplace_back(
                    std::string{
                        impl_mod.ast.get_as<ast::identifier_expr>(impl_stmt->impl_params[i].name)
                            .name},
                    it->second);
            } else {
                auto* arg{ctor_args[*slot]};
                if (!arg || arg->is_poison()) {
                    ok = false;
                    break;
                }
                // Still an unbound param, possibly wrapped (`[]mut T` from a generic signature)
                if (is_generic_type(*arg, false)) { is_abstract = true; }
                if (i < type_bounds.size()) { type_bounds[i] = arg; }
            }
        }
        if (!ok) { continue; }

        // The impl's own module may not have run its template pass yet (a consumer reached this
        // ctor first). Build it now against that module so expansion is order-independent.
        auto tmpl_p{ctx_.impls.get_template(*pimpl)};
        if (!tmpl_p) {
            if (&impl_mod == &resolving_) {
                const scope s{table_stack_,
                              resolving_.get_sema_type(pimpl->site).get_symbol_table_idx(),
                              table_idx_};
                build_param_impl_template(*impl_stmt, pimpl->site);
            } else {
                symbol_table_stack stk;
                stk.push(*ctx_.prelude_index);
                if (impl_mod.root_table_idx) { stk.push(*impl_mod.root_table_idx); }
                stk.push(pimpl->body_scope_idx);
                type_resolver sub{impl_mod, ctx_, pimpl->body_scope_idx, std::move(stk)};
                sub.for_generic_instantiation_ = true;
                sub.build_param_impl_template(*impl_stmt, pimpl->site);
            }
            tmpl_p = ctx_.impls.get_template(*pimpl);
        }
        if (!tmpl_p) { continue; }
        const auto& tmpl{*tmpl_p};
        if (!tmpl.abstract_target || tmpl.sentinels.size() != type_bounds.size()) { continue; }

        // One expansion per (parameterized impl, concrete target).
        if (!ctx_.impls.mark_expanded(concrete, *pimpl)) { continue; }

        // Method signatures are remapped (pure substitution). Method bodies are re-resolved below
        // against real bindings so `if comptime` / dependent types behave as everywhere else.
        const auto remap_one{[&](type* t) -> type* {
            type* r{&remap_type(ctx_, *t, *tmpl.abstract_target, concrete)};
            for (usize i{0}; i < tmpl.sentinels.size() && i < type_bounds.size(); ++i) {
                if (tmpl.sentinels[i] && type_bounds[i]) {
                    r = &remap_type(ctx_, *r, *tmpl.sentinels[i], *type_bounds[i]);
                }
            }
            return r;
        }};

        const auto typing_key{fmt::format("pimpl{}#{}", pimpl->site.get_index(), ctor_mangled)};
        if (!is_abstract) {
            body_type_diff typing;
            resolve_param_impl_bodies(impl_mod,
                                      *impl_stmt,
                                      pimpl->body_scope_idx,
                                      concrete,
                                      type_bounds,
                                      cx_bindings,
                                      typing);

            // Replay each method's remapped `fn` type, keyed by the decl stmt `emit_function` reads
            // and by its `fn` expr
            for (const auto& member : impl_stmt->members) {
                const auto decl{impl_mod.ast.get_as_opt<ast::decl_stmt>(*member)};
                if (!decl || !decl->value || !decl->value->is<ast::function_expr>()) { continue; }
                if (typing.find_node_type(decl->value->get_index())) { continue; }
                if (const auto t{impl_mod.get_sema_type_opt(*decl->value)}) {
                    auto* remapped{remap_one(t.get())};
                    typing.node_types.insert_or_assign((*member).get_index(), remapped);
                    typing.node_types.insert_or_assign(decl->value->get_index(), remapped);
                }
            }

            if (!typing.empty()) {
                ctx_.instantiation_cache.set_body_type_diff(typing_key, std::move(typing));
            }
            if (!cx_bindings.empty()) {
                ctx_.instantiation_cache.set_type_ctor_bindings(typing_key, std::move(cx_bindings));
            }
            ctx_.advance_epoch();
        }

        std::vector<type*> rec_sentinels;
        std::vector<type*> rec_type_args;
        for (usize i{0}; i < tmpl.sentinels.size() && i < type_bounds.size(); ++i) {
            if (tmpl.sentinels[i] && type_bounds[i]) {
                rec_sentinels.emplace_back(tmpl.sentinels[i]);
                rec_type_args.emplace_back(type_bounds[i]);
            }
        }

        impl_record rec{
            .interface_type     = pimpl->interface_type,
            .target_type        = concrete,
            .site               = pimpl->site,
            .enclosing          = pimpl->enclosing,
            .body_scope_idx     = pimpl->body_scope_idx,
            .methods            = {},
            .from_parameterized = true,
            .gir_prefix         = typing_key,
            .sentinels          = std::move(rec_sentinels),
            .type_arguments     = std::move(rec_type_args),
        };
        for (const auto& member : impl_stmt->members) {
            const auto decl{impl_mod.ast.get_as_opt<ast::decl_stmt>(*member)};
            if (!decl || !decl->value || !decl->value->is<ast::function_expr>()) { continue; }
            impl_record::method m{
                .name    = impl_mod.ast.get_as<ast::identifier_expr>(*decl->name).name,
                .decl    = *member,
                .fn_type = stdx::none,
                .is_pub  = decl->has_modifier(ast::decl_modifiers::PUBLIC),
            };
            if (const auto diff{ctx_.instantiation_cache.get_body_type_diff(typing_key)}) {
                if (const auto nt{diff->find_node_type(decl->value->get_index())}; nt && *nt) {
                    m.fn_type = *nt;
                }
            }
            if (!m.fn_type) {
                if (const auto t{impl_mod.get_sema_type_opt(*decl->value)}) {
                    m.fn_type = remap_one(t.get());
                }
            }
            rec.methods.emplace_back(std::move(m));
        }

        const bool trait{pimpl->interface_type.has_value()};
        auto       recorded{ctx_.impls.record(std::move(rec))};
        if (!recorded) {
            ctx_.diags.emplace_back(
                fmt::format("parameterized `impl` yields a duplicate for this instantiation; "
                            "previous impl here: {}",
                            impl_mod.ast.location_of(recorded.error())),
                error::DUPLICATE_IMPL,
                impl_mod.ast.location_of(pimpl->site));
            continue;
        }

        auto* stored{recorded->get()};
        if (trait && stored->interface_type && !is_abstract) {
            if (const auto iface{stored->interface_type->get_data().as_opt<types::interface_t>()}) {
                check_impl_conformance(*stored, *iface);
            }
        }

        // A target holding `type`s only exists at compile time, so its methods have no runtime body
        if (!is_abstract && !holds_type_values(concrete)) {
            for (const auto& m : stored->methods) {
                if (m.fn_type && ctx_.generic_functions.get_opt(*m.fn_type)) { continue; }
                impl_mod.impl_ctor_member_emits.emplace_back<type_ctor_member_emit>({
                    .owner_clone = &concrete,
                    .member_decl = m.decl,
                    .gir_name    = fmt::format("{}.{}", stored->gir_prefix, m.name),
                    .typing_key  = typing_key,
                });
            }
        }
    }
}

auto type_resolver::resolve_param_impl_bodies(
    mod::module_t&                                            impl_mod,
    const ast::impl_stmt&                                     impl,
    usize                                                     body_scope,
    type&                                                     concrete,
    gsl::span<type* const>                                    type_bounds,
    gsl::span<const std::pair<std::string, gir::const_value>> cx_bindings,
    body_type_diff&                                           out) -> void {
    PROFILE_FUNCTION();

    const body_typing_snapshot snap{impl_mod};
    const auto                 restore_guard{gsl::finally([&] { snap.restore_to(impl_mod); })};

    // Binds each impl param for this monomorphization: a type param to its ctor argument type
    // (as a symbol and a `const_eval` frame entry), a `comptime` param to its folded value
    comptime_frame frame;
    for (usize i{0}; i < impl.impl_params.size(); ++i) {
        const auto& pname{impl_mod.ast.get_as<ast::identifier_expr>(impl.impl_params[i].name).name};
        if (impl.impl_params[i].is_comptime) {
            const auto it{std::ranges::find(
                cx_bindings, pname, [](const auto& p) { return std::string_view{p.first}; })};
            if (it != cx_bindings.end()) { frame.insert_or_assign(pname, it->second); }
        } else if (i < type_bounds.size() && type_bounds[i]) {
            auto& concrete_arg{denoted_type(*type_bounds[i])};
            frame.insert_or_assign(pname, gir::const_value{concrete_arg});
            impl_mod.set_sema_type(impl.impl_params[i].name, concrete_arg);
            if (const auto s{ctx_.registry.get_from_opt(body_scope, pname)}) {
                s->set_status(symbol_status::RESOLVED);
            }
        }
    }
    const comptime_frame_guard frame_guard{ctx_.comptime_binding_frames, std::move(frame)};

    for (const auto& member : impl.members) {
        const auto decl{impl_mod.ast.get_as_opt<ast::decl_stmt>(*member)};
        if (!decl || !decl->value || !decl->value->is<ast::function_expr>()) { continue; }
        const auto& fn_expr{impl_mod.ast.get_as<ast::function_expr>(*decl->value)};
        const auto  fn_type{impl_mod.get_sema_type_opt(*decl->value)};
        if (!fn_type || !fn_type->has_symbol_table_idx()) { continue; }
        const auto fn_table{fn_type->get_symbol_table_idx()};

        // A fresh sub-resolver rooted at the member's own scope (under the impl body scope, so
        // impl params resolve), re-resolving params + body like `instantiate_generic`.
        symbol_table_stack stk;
        stk.push(*ctx_.prelude_index);
        if (impl_mod.root_table_idx) { stk.push(*impl_mod.root_table_idx); }
        stk.push(body_scope);
        stk.push(fn_table);
        type_resolver inst{impl_mod, ctx_, fn_table, std::move(stk)};
        inst.for_generic_instantiation_ = true;
        inst.reresolve_floor_.emplace(fn_table);
        inst.instantiation_root_depth_.emplace(inst.table_stack_.size() - 1);
        const structural_guard this_guard{inst.user_type_stack_, concrete};

        const auto mark_resolved{[&](ast::node_id name) {
            if (const auto ident{impl_mod.ast.get_as_opt<ast::identifier_expr>(name)}) {
                if (const auto s{ctx_.registry.get_from_opt(fn_table, ident->name)}) {
                    s->set_kind(symbol_kind::VALUE);
                    s->set_status(symbol_status::RESOLVED);
                }
            }
        }};

        // `self` binds to the monomorphized target, wrapped per its `&` / `^` / `mut` modifier.
        if (fn_expr.self) {
            type* self_t{&concrete};
            if (const auto m{types::mut::from_type_modifier(fn_expr.self->modifier)}) {
                self_t = fn_expr.self->modifier.is_ptr() ? &ctx_.get_pointer(*m, concrete)
                                                         : &ctx_.get_reference(*m, concrete);
            }
            impl_mod.set_sema_type(fn_expr.self->name, *self_t);
            mark_resolved(fn_expr.self->name);
        }

        bool is_generic_method{any_param_comptime(fn_expr)};
        for (const auto& param : fn_expr.parameters) {
            inst.resolve_param_type(param.explicit_type);
            if (inst.last_type_ && !inst.last_type_->is_poison()) {
                auto& pty{denoted_type(*inst.last_type_.take())};
                if (is_generic_type(pty) || param.is_comptime) { is_generic_method = true; }
                impl_mod.set_sema_type(param.name, pty);
            }
            mark_resolved(param.name);
        }

        if (is_generic_method) {
            inst.resolve(fn_expr.explicit_return_type);
            auto& ret{inst.last_type_ && !inst.last_type_->is_poison()
                          ? denoted_type(*inst.last_type_.take())
                          : ctx_.get_builtin_resolved_type(type_kind::VOID_)};

            const bool  has_self{fn_expr.self.has_value()};
            const usize num_params{fn_expr.parameters.size() + (has_self ? 1UZ : 0UZ)};
            auto        concrete_param_types{ctx_.pool.get_many_unsafe(num_params)};
            usize       pi{0};
            if (has_self) {
                if (const auto st{impl_mod.get_sema_type_opt(fn_expr.self->name)}) {
                    concrete_param_types[pi++] = st.get();
                } else {
                    concrete_param_types[pi++] = &concrete;
                }
            }
            for (const auto& param : fn_expr.parameters) {
                if (const auto pt{impl_mod.get_sema_type_opt(param.name)}) {
                    concrete_param_types[pi++] = pt.get();
                } else {
                    concrete_param_types[pi++] = &ctx_.get_poison();
                }
            }

            types::key_t key{type_kind::FUNCTION, types::mut::CONSTANT};
            for (auto* p : concrete_param_types) { key.imprint(*p); }
            key.imprint(ret);
            key.imprint(static_cast<u64>(has_self));
            key.imprint(static_cast<u64>(fn_expr.variadic));
            key.imprint(fn_expr.conv);
            auto& concrete_fn_type{*ctx_.pool[key]};
            concrete_fn_type.resolve_if<types::function>(
                concrete_param_types, ret, has_self, fn_expr.variadic, fn_expr.conv);
            if (fn_type && fn_type->has_symbol_table_idx()) {
                concrete_fn_type.set_symbol_table_idx(fn_type->get_symbol_table_idx());
            }
            impl_mod.set_sema_type(*decl->value, concrete_fn_type);
            impl_mod.set_sema_type(*member, concrete_fn_type);

            const auto& mname{impl_mod.ast.get_as<ast::identifier_expr>(*decl->name).name};
            ctx_.generic_functions.register_function(
                concrete_fn_type, impl_mod, *member, fn_expr, mname, concrete);
            continue;
        }
        inst.resolve(fn_expr.explicit_return_type);
        auto&      ret{inst.last_type_ && !inst.last_type_->is_poison()
                           ? denoted_type(*inst.last_type_.take())
                           : ctx_.get_builtin_resolved_type(type_kind::VOID_)};
        const bool auto_ret{ret.get_kind() == type_kind::AUTO};
        inst.return_trackers_.emplace_back(return_tracker{
            .return_types   = {},
            .is_auto_return = auto_ret,
            .expected_type  = auto_ret ? stdx::none : stdx::option<type&>{ret},
        });
        const auto&              block{impl_mod.ast.get_as<ast::block_stmt>(*fn_expr.body)};
        const active_block_guard guard{inst.active_blocks_, block};
        for (usize idx{0}; idx < block.statements.size(); ++idx) {
            inst.active_blocks_.back().current_stmt_idx = idx;
            inst.resolve(block.statements[idx]);
            if (inst.folded_if_exits(block.statements[idx])) { break; }
        }
        auto tracker{std::move(inst.return_trackers_.back())};
        inst.return_trackers_.pop_back();

        stdx::option<type&> deduced_ret{auto_ret ? tracker.deduced_return_type(ctx_) : ret};
        // If the return type is a type constructor call (e.g. `Flow(T, E)`), force its
        // evaluation at compile time so the monomorphized signature has the concrete union type.
        if (deduced_ret->get_data().is<types::deferred_call>()) {
            gir::const_eval evaluator{ctx_, impl_mod};
            deduced_ret.emplace(denoted_type(evaluator.force_deferred_call(*deduced_ret)));
        }

        // The monomorphized method's `types::function`, with `self` first when `has_self` so arity
        // and conformance checks see the receiver
        const bool  has_self{fn_expr.self.has_value()};
        const usize num_params{fn_expr.parameters.size() + (has_self ? 1UZ : 0UZ)};
        auto        concrete_param_types{ctx_.pool.get_many_unsafe(num_params)};
        usize       pi{0};
        if (has_self) {
            if (const auto st{impl_mod.get_sema_type_opt(fn_expr.self->name)}) {
                concrete_param_types[pi++] = st.get();
            } else {
                concrete_param_types[pi++] = &concrete;
            }
        }
        for (const auto& param : fn_expr.parameters) {
            if (const auto pt{impl_mod.get_sema_type_opt(param.name)}) {
                concrete_param_types[pi++] = pt.get();
            } else {
                concrete_param_types[pi++] = &ctx_.get_poison();
            }
        }

        types::key_t key{type_kind::FUNCTION, types::mut::CONSTANT};
        for (auto* p : concrete_param_types) { key.imprint(*p); }
        key.imprint(*deduced_ret);
        key.imprint(static_cast<u64>(fn_expr.self.has_value()));
        key.imprint(static_cast<u64>(fn_expr.variadic));
        key.imprint(fn_expr.conv);
        auto& concrete_fn_type{*ctx_.pool[key]};
        concrete_fn_type.resolve_if<types::function>(concrete_param_types,
                                                     *deduced_ret,
                                                     fn_expr.self.has_value(),
                                                     fn_expr.variadic,
                                                     fn_expr.conv);
        if (fn_type && fn_type->has_symbol_table_idx()) {
            concrete_fn_type.set_symbol_table_idx(fn_type->get_symbol_table_idx());
        }
        impl_mod.set_sema_type(*decl->value, concrete_fn_type);
        impl_mod.set_sema_type(*member, concrete_fn_type);
    }
    snap.diff_into(ctx_, impl_mod, out);
}

auto type_resolver::member_function_is_poisoned(ast::member_handle member) const -> bool {
    const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(member)};
    if (!decl || !decl->value || !decl->value->is<ast::function_expr>()) { return false; }
    const auto fn_type{resolving_.get_sema_type_opt(*decl->value)};
    return !fn_type || fn_type->is_poison();
}

auto type_resolver::build_param_impl_template(const ast::impl_stmt& impl, ast::node_id site)
    -> void {
    PROFILE_FUNCTION();
    // `pimpl` is absent for an unanchored `impl(P) BareType` (no base ctor to key on). We still
    // resolve the body here so its symbols settle. There is just nothing to expand later.
    const auto pimpl{ctx_.impls.param_record_by_site(site, resolving_)};
    if (pimpl) {
        if (ctx_.impls.get_template(*pimpl)) { return; }
        // Resolving the abstract `Ctor(...)` target below re-enters expansion for this same
        // impl. Bail on that recursive entry. Cleared once the template is stored.
        if (!ctx_.impls.begin_build(*pimpl)) { return; }
    }

    // Bind each type param to its own opaque sentinel `type` and each `comptime` param to a
    // dummy value, then resolve the target + members once
    std::vector<type*> sentinels;
    sentinels.reserve(impl.impl_params.size());
    comptime_frame cx_dummy;
    for (const auto& p : impl.impl_params) {
        if (p.is_comptime) {
            resolve_param_type(p.explicit_type);
            auto& pty{last_type_ && !last_type_->is_poison()
                          ? denoted_type(*last_type_)
                          : ctx_.get_builtin_resolved_type(type_kind::USIZE)};
            resolving_.set_sema_type(p.name, pty);
            resolve_symbol_info(p.name, symbol_kind::VALUE);
            sentinels.emplace_back(nullptr);
            cx_dummy.insert_or_assign(resolving_.ast.get_as<ast::identifier_expr>(p.name).name,
                                      gir::const_value{u64{1}, pty});
        } else {
            auto& sentinel{param_impl_sentinel((*p.name).get_index())};
            resolving_.set_sema_type(p.name, sentinel);
            resolve_symbol_info(p.name, symbol_kind::TYPE);
            sentinels.emplace_back(&sentinel);
            // Without this, a `type`-argument fold  finds no binding for `T` in this frame
            cx_dummy.insert_or_assign(resolving_.ast.get_as<ast::identifier_expr>(p.name).name,
                                      gir::const_value{sentinel});
        }
    }
    const comptime_frame_guard cx_guard{ctx_.comptime_binding_frames, std::move(cx_dummy)};
    ctx_.advance_epoch();

    // The sentinel/dummy resolution only has to yield the abstract target + a signature per
    // method. Its diags are noise and its `if comptime` folds are redone per instantiation.
    const auto                   snap_ifs{resolving_.if_comptime_results};
    const auto                   snap_matches{resolving_.match_arm_results};
    const auto                   diags_before{ctx_.diags.size()};
    const mutating_context_guard tmode{building_param_template_, true};

    if (impl.interface_type) { resolve(*impl.interface_type); }
    stdx::option<type&>            abstract_target;
    stdx::option<structural_guard> guard;
    resolve(impl.target_type);
    if (last_type_ && !last_type_->is_poison()) {
        auto& t{denoted_type(*last_type_)};
        abstract_target.emplace(t);
        guard.emplace(user_type_stack_, t);
    }
    // A method left poisoned here is skipped by every instantiation, so its diags are real
    std::vector<std::pair<usize, usize>> poisoned_member_diags;
    for (const auto& member : impl.members) {
        const auto member_diags_begin{ctx_.diags.size()};
        resolve(*member);
        if (member_function_is_poisoned(member)) {
            poisoned_member_diags.emplace_back(member_diags_begin - diags_before,
                                               ctx_.diags.size() - diags_before);
        }
    }

    // ...as is a name that resolves nowhere, which no instantiation's arguments can fix
    const auto discarded{ctx_.diags.split_off(diags_before)};
    const auto kept_for_poisoned_member{[&](usize idx) {
        return std::ranges::any_of(poisoned_member_diags, [idx](const auto& range) {
            return idx >= range.first && idx < range.second;
        });
    }};
    for (usize idx{0}; const auto& diag : static_cast<gsl::span<const diagnostic>>(discarded)) {
        if (diag.get_error() == error::UNDECLARED_IDENTIFIER || kept_for_poisoned_member(idx)) {
            ctx_.diags.emplace_back(diag);
        }
        idx += 1;
    }
    resolving_.if_comptime_results = snap_ifs;
    resolving_.match_arm_results   = snap_matches;
    ctx_.advance_epoch();

    if (!pimpl) { return; } // unanchored: members resolved, nothing to store
    ctx_.impls.set_template(
        *pimpl,
        param_impl_template{.abstract_target = abstract_target, .sentinels = std::move(sentinels)});
    ctx_.impls.end_build(*pimpl);
}

auto type_resolver::visit(ast::node_id id, const ast::impl_stmt& impl) -> void {
    PROFILE_FUNCTION();
    auto&       impl_type{resolving_.get_sema_type(id)};
    const scope s{table_stack_, impl_type.get_symbol_table_idx(), table_idx_};
    auto&       void_type{ctx_.get_builtin_resolved_type(type_kind::VOID_)};

    // Parameterized `impl(P) ...` is re-instantiated per monomorphization of its target.
    if (!impl.impl_params.empty()) {
        build_param_impl_template(impl, id);
        return last_type_.emplace(void_type);
    }

    const auto rec{ctx_.impls.find_by_site(id, resolving_)};

    if (impl.interface_type) {
        TRY_RESOLVE(*impl.interface_type);
        auto& iface_type{denoted_type(*last_type_)};
        // A not-yet-folded type-constructor result is checked once it resolves
        const auto iface_kind{iface_type.get_kind()};
        if (iface_kind != type_kind::INTERFACE && iface_kind != type_kind::TYPE &&
            !iface_type.is_poison()) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("`impl X for T` requires X to be an interface; found `{}`",
                            ctx_.type_display_name(iface_type)),
                error::TYPE_MISMATCH,
                resolving_.ast.location_of(*impl.interface_type)));
        }
    }
    TRY_RESOLVE(impl.target_type);
    auto& target{denoted_type(*last_type_)};

    {
        stdx::option<structural_guard> guard;
        if (!target.is_poison()) { guard.emplace(user_type_stack_, target); }
        for (const auto& member : impl.members) {
            TRY_RESOLVE(*member);
            // Publish each method's resolved type as soon as it is available so a later
            // sibling's body can call it through `self.<name>(...)`
            if (rec) {
                for (auto& m : rec->methods) {
                    if (m.fn_type) { continue; }
                    if (const auto t{resolving_.get_sema_type_opt(m.decl)}) {
                        m.fn_type.emplace(*t);
                    }
                }
            }
        }
    }

    if (rec) {
        // Fill any method type still missing
        for (auto& m : rec->methods) {
            if (m.inherited) { continue; }
            if (const auto t{resolving_.get_sema_type_opt(m.decl)}) { m.fn_type.emplace(*t); }
        }
        if (rec->interface_type && !target.is_poison()) {
            if (const auto iface{rec->interface_type->get_data().as_opt<types::interface_t>()}) {
                check_impl_conformance(*rec, *iface);
                resolve_inherited_default_methods(*rec, *iface);
            }
        }
    }

    last_type_.emplace(void_type);
}

auto type_resolver::types_match_with_assoc(const impl_record&        rec,
                                           const types::interface_t& iface,
                                           const type&               want,
                                           const type&               have) -> bool {
    if (&want == &have || is_same_unqualified(want, have) || is_assignable(have, want)) {
        return true;
    }

    // Check if `want` is an associated type placeholder of `iface`:
    for (usize k{0}; k < iface.ast_assoc_types.size(); ++k) {
        const auto node_idx{(*iface.ast_assoc_types[k].name).get_index()};
        if (&want == &assoc_type_placeholder(node_idx)) {
            const auto assoc_name{iface.assoc_type_names[k]};
            if (const auto bound{find_assoc_type_alias(ctx_, rec, assoc_name)}) {
                if (bound->get_kind() == type_kind::TYPE) { return true; }
                return is_same_unqualified(*bound, have) || is_assignable(have, *bound);
            }
            return true;
        }
    }

    const auto get_arg_type{
        [](const mod::module_t& m, const ast::call_expr::argument& arg) -> stdx::option<type&> {
            if (const auto tid{arg.as_opt<ast::explicit_type_id>()}) {
                return m.get_sema_type_opt(*tid);
            }
            if (const auto eh{arg.as_opt<ast::expr_handle>()}) { return m.get_sema_type_opt(*eh); }
            return stdx::none;
        }};

    if (const auto dc_w{want.get_data().as_opt<types::deferred_call>()}) {
        if (const auto dc_h{have.get_data().as_opt<types::deferred_call>()}) {
            if (dc_w->call.arguments.size() == dc_h->call.arguments.size()) {
                bool        args_match{true};
                const auto& mod_w{iface.enclosing};
                const auto& mod_h{rec.enclosing ? *rec.enclosing : resolving_};
                for (usize i{0}; i < dc_w->call.arguments.size(); ++i) {
                    const auto arg_w{get_arg_type(mod_w, dc_w->call.arguments[i])};
                    const auto arg_h{get_arg_type(mod_h, dc_h->call.arguments[i])};
                    if (arg_w && arg_h) {
                        if (!types_match_with_assoc(rec, iface, *arg_w, *arg_h)) {
                            args_match = false;
                            break;
                        }
                    }
                }
                if (args_match) { return true; }
            }
        }
    }

    if (want.get_kind() == type_kind::TYPE) { return true; }

    if (const auto pw{want.get_data().as_opt<types::pointer>()}) {
        if (const auto ph{have.get_data().as_opt<types::pointer>()}) {
            if (want.is_constant() != have.is_constant()) { return false; }
            return types_match_with_assoc(rec, iface, pw->underlying, ph->underlying);
        }
        return false;
    }

    if (const auto rw{want.get_data().as_opt<types::reference>()}) {
        if (const auto rh{have.get_data().as_opt<types::reference>()}) {
            if (want.is_constant() != have.is_constant()) { return false; }
            return types_match_with_assoc(rec, iface, rw->underlying, rh->underlying);
        }
        return false;
    }

    if (const auto sw{want.get_data().as_opt<types::slice>()}) {
        if (const auto sh{have.get_data().as_opt<types::slice>()}) {
            if (want.is_constant() != have.is_constant() ||
                sw->null_terminated != sh->null_terminated) {
                return false;
            }
            return types_match_with_assoc(rec, iface, sw->underlying, sh->underlying);
        }
        return false;
    }

    if (const auto aw{want.get_data().as_opt<types::array>()}) {
        if (const auto ah{have.get_data().as_opt<types::array>()}) {
            if (want.is_constant() != have.is_constant() || aw->len != ah->len ||
                aw->null_terminated != ah->null_terminated) {
                return false;
            }
            return types_match_with_assoc(rec, iface, aw->underlying, ah->underlying);
        }
        return false;
    }

    if (const auto uw{want.get_data().as_opt<types::union_t>()}) {
        if (const auto uh{have.get_data().as_opt<types::union_t>()}) {
            if (uw->fields.size() != uh->fields.size()) { return false; }
            for (usize i{0}; i < uw->fields.size(); ++i) {
                const auto nw{
                    uw->enclosing.ast.get_as<ast::identifier_expr>(*uw->ast_fields[i].name).name};
                const auto nh{
                    uh->enclosing.ast.get_as<ast::identifier_expr>(*uh->ast_fields[i].name).name};
                if (nw != nh) { return false; }
                if (!types_match_with_assoc(rec, iface, *uw->fields[i], *uh->fields[i])) {
                    return false;
                }
            }
            return true;
        }
        return false;
    }

    if (const auto sw{want.get_data().as_opt<types::struct_t>()}) {
        if (const auto sh{have.get_data().as_opt<types::struct_t>()}) {
            if (sw->fields.size() != sh->fields.size()) { return false; }
            for (usize i{0}; i < sw->fields.size(); ++i) {
                const auto nw{
                    sw->enclosing.ast.get_as<ast::identifier_expr>(*sw->ast_fields[i].name).name};
                const auto nh{
                    sh->enclosing.ast.get_as<ast::identifier_expr>(*sh->ast_fields[i].name).name};
                if (nw != nh) { return false; }
                if (!types_match_with_assoc(rec, iface, *sw->fields[i], *sh->fields[i])) {
                    return false;
                }
            }
            return true;
        }
        return false;
    }

    if (const auto fw{want.get_data().as_opt<types::function>()}) {
        if (const auto fh{have.get_data().as_opt<types::function>()}) {
            if (fw->has_self != fh->has_self || fw->is_variadic != fh->is_variadic ||
                fw->params.size() != fh->params.size()) {
                return false;
            }
            for (usize i{0}; i < fw->params.size(); ++i) {
                if (!types_match_with_assoc(rec, iface, *fw->params[i], *fh->params[i])) {
                    return false;
                }
            }
            return types_match_with_assoc(rec, iface, fw->return_type, fh->return_type);
        }
        return false;
    }

    return false;
}

auto type_resolver::check_impl_conformance(const impl_record& rec, const types::interface_t& iface)
    -> void {
    PROFILE_FUNCTION();
    // A parameterized impl may be conformance-checked while a different module is being
    // resolved (at the point its target first monomorphizes), so read AST from where it lives.
    const auto& cmod{rec.enclosing ? *rec.enclosing : resolving_};
    const auto  site_loc{cmod.ast.location_of(rec.site)};
    const auto  iface_name{ctx_.type_display_name(*rec.interface_type)};
    const auto  target_name{ctx_.type_display_name(*rec.target_type)};
    const auto& body_table{ctx_.registry.get(rec.body_scope_idx)};

    for (usize i{0}; i < iface.requirement_count; ++i) {
        const auto name{iface.method_names[i]};
        const auto supplied{rec.find_method(name)};
        if (!supplied) {
            ctx_.diags.emplace_back(fmt::format("`{}` does not implement `{}`: missing method `{}`",
                                                target_name,
                                                iface_name,
                                                name),
                                    error::MISSING_IMPL_METHOD,
                                    site_loc);
            continue;
        }
        const auto method_loc{cmod.ast.location_of(supplied->decl)};
        const auto expected{iface.method_sigs[i]->get_data().as_opt<types::function>()};
        const auto got{supplied->fn_type ? supplied->fn_type->get_data().as_opt<types::function>()
                                         : stdx::none};
        if (!expected || !got) { continue; }

        if (expected->has_self != got->has_self || expected->params.size() != got->params.size() ||
            expected->is_variadic != got->is_variadic) {
            ctx_.diags.emplace_back(
                fmt::format("method `{}` does not match the signature required by `{}` "
                            "(parameter count or `self` differs)",
                            name,
                            iface_name),
                error::IMPL_SIGNATURE_MISMATCH,
                method_loc);
            continue;
        }
        if (expected->has_self && !self_binding_compatible(*expected->params[0], *got->params[0])) {
            ctx_.diags.emplace_back(
                fmt::format(
                    "method `{}`: `self` binding is not compatible with the requirement in `{}`",
                    name,
                    iface_name),
                error::IMPL_SELF_MISMATCH,
                method_loc);
        }

        const usize first_param{expected->has_self ? 1UZ : 0UZ};
        for (usize p{first_param}; p < expected->params.size(); ++p) {
            auto& want_base{*expected->params[p]};
            auto& want{rec.interface_type && rec.target_type
                           ? remap_type(ctx_, want_base, *rec.interface_type, *rec.target_type)
                           : want_base};
            auto& have{*got->params[p]};
            if (!types_match_with_assoc(rec, iface, want, have) ||
                contains_poly(want) != contains_poly(have)) {
                ctx_.diags.emplace_back(
                    fmt::format(
                        "method `{}`: parameter {} type does not match the requirement in `{}`",
                        name,
                        p - first_param + 1,
                        iface_name),
                    error::IMPL_SIGNATURE_MISMATCH,
                    method_loc);
            }
        }

        auto& expected_ret{
            rec.interface_type && rec.target_type
                ? remap_type(ctx_, expected->return_type, *rec.interface_type, *rec.target_type)
                : expected->return_type};
        if (!types_match_with_assoc(rec, iface, expected_ret, got->return_type) ||
            contains_poly(expected_ret) != contains_poly(got->return_type)) {
            ctx_.diags.emplace_back(
                fmt::format("method `{}`: return type does not match the requirement in `{}`",
                            name,
                            iface_name),
                error::IMPL_SIGNATURE_MISMATCH,
                method_loc);
        }
    }

    // Every required associated type must be bound by the impl or defaulted by the interface.
    for (usize i{0}; i < iface.assoc_type_names.size(); ++i) {
        const bool defaulted{iface.ast_assoc_types[i].default_type.has_value()};
        if (!defaulted && !body_table.has(iface.assoc_type_names[i])) {
            ctx_.diags.emplace_back(
                fmt::format("`{}` does not implement `{}`: associated type `{}` is not bound",
                            target_name,
                            iface_name,
                            iface.assoc_type_names[i]),
                error::MISSING_IMPL_METHOD,
                site_loc);
        }
    }

    // A  static `let mut` is allowed for 'global' state but other members are not allowed
    for (const auto& entry : body_table) {
        const auto member_name{entry.first};

        // A parameterized `impl(P) ...` keeps its type params in this same scope
        const auto node{entry.second.symbol.get_data().as_opt<symbols::node_t>()};
        if (!node) { continue; }

        bool is_item{false};
        is_item |= std::ranges::contains(iface.method_names, member_name);
        is_item |= std::ranges::contains(iface.assoc_type_names, member_name);
        is_item |= std::ranges::contains(iface.assoc_const_names, member_name);

        const auto decl{cmod.ast.get_as_opt<ast::decl_stmt>(*node)};
        if (decl && decl->has_modifier(ast::decl_modifiers::MUT)) { is_item = true; }

        if (!is_item) {
            ctx_.diags.emplace_back(
                fmt::format(
                    "`{}` is not a member of interface `{}`; put unrelated items in an inherent "
                    "`impl {}` block",
                    member_name,
                    iface_name,
                    target_name),
                error::UNKNOWN_IMPL_MEMBER,
                site_loc);
        }
    }
}

auto type_resolver::resolve_inherited_default_methods(impl_record&              rec,
                                                      const types::interface_t& iface) -> void {
    PROFILE_FUNCTION();
    if (iface.method_names.size() <= iface.requirement_count) { return; }
    if (!rec.interface_type || !rec.target_type) { return; }

    auto&       imod{iface.enclosing};
    const auto  iface_scope{rec.interface_type->get_symbol_table_idx()};
    auto&       target{*rec.target_type};
    const auto& impl_mod{rec.enclosing ? *rec.enclosing : resolving_};

    // Rebind the interface's associated types to what this impl supplies
    struct saved_assoc {
        gsl::not_null<symbol*> sym;
        ast::identifier_handle name_node;
        stdx::option<type&>    prev_type;
        symbol_status          prev_status;
    };

    std::vector<saved_assoc> restore;
    for (usize k{0}; k < iface.assoc_type_names.size() && k < iface.ast_assoc_types.size(); ++k) {
        const auto name{iface.assoc_type_names[k]};
        const auto bsym{ctx_.registry.get_from_opt(rec.body_scope_idx, name)};
        if (!bsym) { continue; } // defaulted associated type; leave the interface default
        const auto bnode{bsym->get_data().as_opt<symbols::node_t>()};
        if (!bnode) { continue; }

        stdx::option<type&> bound;
        if (const auto bd{impl_mod.ast.get_as_opt<ast::decl_stmt>(*bnode)}; bd && bd->value) {
            bound = impl_mod.get_sema_type_opt(*bd->value);
        }
        if (!bound || !bound->is_resolved()) { continue; }

        const auto isym{ctx_.registry.get_from_opt(iface_scope, name)};
        if (!isym) { continue; }
        const auto at_name{iface.ast_assoc_types[k].name};
        restore.emplace_back<saved_assoc>({
            .sym         = isym.get(),
            .name_node   = at_name,
            .prev_type   = imod.get_sema_type_opt(at_name),
            .prev_status = isym->get_status(),
        });
        imod.set_sema_type(at_name, denoted_type(*bound));
        isym->set_status(symbol_status::RESOLVED);
    }

    const auto restore_assoc{gsl::finally([&] {
        for (const auto& s : restore) {
            s.sym->set_status(s.prev_status);
            if (s.prev_type) { imod.set_sema_type(s.name_node, *s.prev_type); }
        }
    })};

    for (usize i{iface.requirement_count}; i < iface.method_names.size(); ++i) {
        const auto name{iface.method_names[i]};
        if (rec.find_method(name)) { continue; } // overridden by an explicit `impl` member

        const auto& m{iface.method_decl(i)};
        const auto  sig_node{ast::node_id{*m.signature}};
        const auto& fn_expr{imod.ast.get_as<ast::function_expr>(*m.signature)};
        if (fn_expr.is_type_expr) { continue; } // a bodyless requirement, not a default

        const auto sig_ty{imod.get_sema_type_opt(*m.signature)};
        if (!sig_ty || !sig_ty->has_symbol_table_idx()) { continue; }
        const auto fn_table{sig_ty->get_symbol_table_idx()};

        // Every `impl` shares the interface's signature node, so it briefly takes this target's
        // concrete `fn` type and the default body re-resolves against it
        auto&      abstract_sig{*sig_ty};
        const auto restore_sig{
            gsl::finally([&] { imod.set_sema_type(*m.signature, abstract_sig); })};

        symbol_table_stack stk;
        stk.push(*ctx_.prelude_index);
        if (imod.root_table_idx) { stk.push(*imod.root_table_idx); }
        stk.push(iface_scope);
        stk.push(fn_table);
        type_resolver inst{imod, ctx_, fn_table, std::move(stk)};
        inst.for_generic_instantiation_ = true;
        inst.reresolve_floor_.emplace(fn_table);
        inst.instantiation_root_depth_.emplace(inst.table_stack_.size() - 1);

        const structural_guard     this_guard{inst.user_type_stack_, target};
        const body_typing_snapshot snap{imod};

        const auto mark_resolved{[&](ast::node_id nm) {
            if (const auto ident{imod.ast.get_as_opt<ast::identifier_expr>(nm)}) {
                if (const auto s{ctx_.registry.get_from_opt(fn_table, ident->name)}) {
                    s->set_kind(symbol_kind::VALUE);
                    s->set_status(symbol_status::RESOLVED);
                }
            }
        }};

        auto  params{ctx_.pool.get_many_unsafe(fn_expr.parameters.size() + (fn_expr.self ? 1 : 0))};
        usize pi{0};
        if (fn_expr.self) {
            type* self_t{&target};
            if (const auto mut{types::mut::from_type_modifier(fn_expr.self->modifier)}) {
                self_t = fn_expr.self->modifier.is_ptr() ? &ctx_.get_pointer(*mut, target)
                                                         : &ctx_.get_reference(*mut, target);
            }
            imod.set_sema_type(fn_expr.self->name, *self_t);
            mark_resolved(fn_expr.self->name);
            params[pi++] = self_t;
        }

        for (const auto& param : fn_expr.parameters) {
            inst.resolve_param_type(param.explicit_type);
            auto& pt{inst.last_type_ && !inst.last_type_->is_poison()
                         ? denoted_type(*inst.last_type_.take())
                         : ctx_.get_poison()};
            imod.set_sema_type(param.name, pt);
            mark_resolved(param.name);
            params[pi++] = &pt;
        }

        inst.resolve(fn_expr.explicit_return_type);
        auto& ret{inst.last_type_ && !inst.last_type_->is_poison()
                      ? denoted_type(*inst.last_type_.take())
                      : ctx_.get_builtin_resolved_type(type_kind::VOID_)};

        types::key_t key{type_kind::FUNCTION, types::mut::CONSTANT};
        for (const auto* p : params) { key.imprint(*p); }
        key.imprint(ret);
        key.imprint(fn_expr.conv);
        auto& concrete_fn{*ctx_.pool[key]};
        concrete_fn.resolve_if<types::function>(
            params, ret, fn_expr.self.has_value(), fn_expr.variadic, fn_expr.conv);
        imod.set_sema_type(*m.signature, concrete_fn);

        const bool auto_ret{ret.get_kind() == type_kind::AUTO};
        inst.return_trackers_.emplace_back<return_tracker>({
            .return_types   = {},
            .is_auto_return = auto_ret,
            .expected_type  = auto_ret ? stdx::none : stdx::option<type&>{ret},
        });
        // `sig_node` now carries the per-target concrete signature, so a `?` in the body reads the
        // right return type off it.
        const open_function_guard body_fn_guard{inst.open_function_nodes_, sig_node};
        for (const auto& stmt : imod.ast.get_as<ast::block_stmt>(*fn_expr.body)) {
            inst.resolve(stmt);
        }
        inst.return_trackers_.pop_back();

        body_type_diff diff;
        snap.diff_into(ctx_, imod, diff);

        auto typing_key{fmt::format("impldef{}#{}", rec.body_scope_idx, name)};
        if (!diff.empty()) {
            ctx_.instantiation_cache.set_body_type_diff(typing_key, std::move(diff));
        }

        rec.methods.emplace_back<impl_record::method>({
            .name         = name,
            .decl         = sig_node,
            .fn_type      = concrete_fn,
            .is_pub       = iface.method_is_pub[i],
            .inherited    = true,
            .typing_key   = std::move(typing_key),
            .defining_mod = imod,
            .signature    = sig_node,
        });
    }
}

auto type_resolver::resolve_dyn_method_signature(const types::dyn_t&       dyn,
                                                 const types::interface_t& iface,
                                                 usize                     method_idx) -> type& {
    PROFILE_FUNCTION();
    auto& fallback{*iface.method_sigs[method_idx]};

    const auto& m{iface.method_decl(method_idx)};
    auto&       imod{iface.enclosing};
    const auto& fn_expr{imod.ast.get_as<ast::function_expr>(*m.signature)};

    // Rebind each associated type straight to this `dyn`'s concrete binding for re-resolution.
    struct saved_assoc {
        gsl::not_null<symbol*> sym;
        ast::identifier_handle name_node;
        stdx::option<type&>    prev_type;
        symbol_status          prev_status;
    };
    std::vector<saved_assoc> restore;
    const auto               iface_scope{dyn.interface.get_symbol_table_idx()};
    for (usize k{0}; k < iface.assoc_type_names.size() && k < iface.ast_assoc_types.size(); ++k) {
        if (k >= dyn.assoc_bindings.size() || !dyn.assoc_bindings[k]) { continue; }
        const auto name{iface.assoc_type_names[k]};
        const auto isym{ctx_.registry.get_from_opt(iface_scope, name)};
        if (!isym) { continue; }
        const auto at_name{iface.ast_assoc_types[k].name};
        restore.emplace_back<saved_assoc>({
            .sym         = isym.get(),
            .name_node   = at_name,
            .prev_type   = imod.get_sema_type_opt(at_name),
            .prev_status = isym->get_status(),
        });
        imod.set_sema_type(at_name, denoted_type(*dyn.assoc_bindings[k]));
        isym->set_status(symbol_status::RESOLVED);
    }
    const auto restore_assoc{gsl::finally([&] {
        for (const auto& s : restore) {
            s.sym->set_status(s.prev_status);
            if (s.prev_type) { imod.set_sema_type(s.name_node, *s.prev_type); }
        }
    })};

    // A required method's own `function_expr` node is never given a sema type
    symbol_table_stack stk;
    stk.push(*ctx_.prelude_index);
    if (imod.root_table_idx) { stk.push(*imod.root_table_idx); }
    stk.push(iface_scope);
    type_resolver inst{imod, ctx_, iface_scope, std::move(stk)};
    inst.for_generic_instantiation_ = true;
    inst.reresolve_floor_.emplace(iface_scope);

    const body_typing_snapshot snap{imod};
    const auto                 fallback_fn{fallback.get_data().as_opt<types::function>()};
    auto  params{ctx_.pool.get_many_unsafe(fn_expr.parameters.size() + (fn_expr.self ? 1 : 0))};
    usize pi{0};
    if (fn_expr.self) {
        // `self` isn't part of what a `dyn`'s associated-type binding can affect
        params[pi++] =
            fallback_fn && !fallback_fn->params.empty() ? fallback_fn->params[0] : &fallback;
    }
    for (const auto& param : fn_expr.parameters) {
        inst.resolve_param_type(param.explicit_type);
        auto& pt{inst.last_type_ && !inst.last_type_->is_poison()
                     ? denoted_type(*inst.last_type_.take())
                     : ctx_.get_poison()};
        params[pi++] = &pt;
    }

    inst.resolve(fn_expr.explicit_return_type);
    auto& ret{inst.last_type_ && !inst.last_type_->is_poison()
                  ? denoted_type(*inst.last_type_.take())
                  : ctx_.get_builtin_resolved_type(type_kind::VOID_)};

    types::key_t key{type_kind::FUNCTION, types::mut::CONSTANT};
    for (const auto* p : params) { key.imprint(*p); }
    key.imprint(ret);
    key.imprint(fn_expr.conv);
    auto& concrete_fn{*ctx_.pool[key]};
    concrete_fn.resolve_if<types::function>(
        params, ret, fn_expr.self.has_value(), fn_expr.variadic, fn_expr.conv);

    snap.restore_to(imod);
    return concrete_fn;
}

} // namespace ghoti::sema
