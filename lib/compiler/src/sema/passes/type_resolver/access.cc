#include <algorithm>
#include <concepts>
#include <string_view>
#include <utility>

#include <gsl/pointers>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/format.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/traits.hh"
#include "compiler/ast/visitor.hh"
#include "compiler/gir/const_eval.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/passes/type_resolver.hh"
#include "compiler/sema/range_steps.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/token_type.hh"
#include "internal.hh"
#include "support/diagnostic.hh"
#include "support/int128.hh"

namespace ghoti::sema {

using namespace resolver_detail;

// `rest[k]`: the k-th argument's own type, `k` a compile-time constant in range.
auto type_resolver::resolve_pack_index(ast::node_id           id,
                                       const ast::index_expr& index,
                                       const pack_binding&    pack) -> void {
    if (resolving_.ast.get_as_opt<ast::range_expr>(index.index)) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            fmt::format("A pack slice has no value of its own; expand it as '{}[lo..hi]...'",
                        pack.name),
            error::MALFORMED_PACK_USE,
            resolving_.ast.location_of(index.index)));
    }
    gir::const_eval evaluator{ctx_, resolving_};
    const auto      folded{evaluator.try_eval(index.index)};
    const auto      k{folded ? folded->as_int_opt() : stdx::none};
    if (!k) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            fmt::format("'{}[k]' requires a compile-time constant index", pack.name),
            error::PACK_INDEX_NOT_CONST,
            resolving_.ast.location_of(index.index)));
    }
    if (*k < 0 || static_cast<usize>(*k) >= pack.element_types.size()) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("pack index {} out of range for '{}' (len {})",
                                         *k,
                                         pack.name,
                                         pack.element_types.size()),
                             error::PACK_INDEX_OUT_OF_RANGE,
                             resolving_.ast.location_of(index.index)));
    }
    auto& elem_type{*pack.element_types[static_cast<usize>(*k)]};
    resolving_.set_sema_type(id, elem_type);
    last_type_.emplace(elem_type);
}

template <typename Eval>
auto type_resolver::fold_type_read(const type& object_type, type& read_type, Eval&& eval) -> type& {
    const auto& read_data{read_type.get_data()};
    if (read_type.get_kind() != type_kind::TYPE || read_data.is<types::meta_type>() ||
        read_data.is<types::deferred_call>() || read_data.is<types::deferred_array>()) {
        return read_type;
    }
    gsl::not_null<const type*> object{&object_type};
    if (const auto ref{object->get_data().as_opt<types::reference>()}) {
        object = &ref->underlying;
    }
    if (!is_comptime_aggregate(*object)) { return read_type; }

    gir::const_eval evaluator{ctx_, resolving_};
    const auto      cv{std::forward<Eval>(eval)(evaluator)};
    const auto      folded{cv ? cv->template as_opt<stdx::option<type&>>() : stdx::none};
    if (!folded || !*folded) { return read_type; }
    auto& instance{**folded};
    auto  meta{ctx_.pool[{type_kind::TYPE, types::mut::CONSTANT, instance}]};
    meta->template resolve_if<types::meta_type>(instance);
    return *meta;
}

auto type_resolver::visit(ast::node_id id, const ast::index_expr& index) -> void {
    PROFILE_FUNCTION();
    if (const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(index.array)}) {
        if (const auto pack{pack_named(ident->name)}) {
            capture_pack_use(*pack);
            return resolve_pack_index(id, index, *pack);
        }
    }
    TRY_RESOLVE(index.array);
    auto&         array_type{*last_type_.take()};
    gsl::not_null target_type{&array_type};
    if (const auto ref{target_type->get_data().as_opt<types::reference>()}) {
        target_type = &ref->underlying;
    }
    auto& array_data{target_type->get_data()};

    if (const auto slice{array_data.as_opt<types::slice>()}) {
        last_type_.emplace(slice->underlying);
    } else if (const auto array{array_data.as_opt<types::array>()}) {
        last_type_.emplace(array->underlying);
    } else if (const auto vec{array_data.as_opt<types::vector>()}) {
        if (is_range(index.index)) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("A vector can't be sliced; convert '{}' to an array first",
                            ctx_.type_display_name(*target_type)),
                error::TYPE_MISMATCH,
                resolving_.ast.location_of(index.index)));
        }
        last_type_.emplace(vec->underlying);
    } else if (const auto deferred{array_data.as_opt<types::deferred_array>()}) {
        last_type_.emplace(deferred->underlying);
    } else if (const auto pointer{array_data.as_opt<types::pointer>()}) {
        last_type_.emplace(pointer->underlying);
    } else {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("Can only index slices, arrays, and pointers; found '{}'",
                                         ctx_.type_display_name(array_type)),
                             error::TYPE_MISMATCH,
                             resolving_.ast.location_of(index.array)));
    }

    auto& single_item_type{*last_type_.take()};

    // An open-ended range index (`x[lo..]`, `x[..hi]`, `x[..]`) needs `operand.len`, which a bare
    // pointer does not have.
    if (const auto range{resolving_.ast.get_as_opt<ast::range_expr>(index.index)};
        range && (!range->lhs || !range->rhs) && array_data.is<types::pointer>()) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "An open-ended range needs a known length; index a pointer with an "
                             "explicit `lo..hi` range instead",
                             error::ILLEGAL_OPEN_RANGE,
                             resolving_.ast.location_of(index.index)));
    }

    {
        auto&                        usize_type{ctx_.get_builtin_resolved_type(type_kind::USIZE)};
        const structural_guard       g{implicit_type_stack_, usize_type};
        const mutating_context_guard subscript_g{in_subscript_index_, is_range(index.index)};
        TRY_RESOLVE(index.index);
    }
    auto& access_type{*last_type_.take()};
    if (!index.index.is<ast::range_expr>() &&
        operand_nature(index.index) == operand_nature_t::TYPE) {
        return last_type_.emplace(ctx_.poison_node(resolving_,
                                                   id,
                                                   "An index must be a value, not a type",
                                                   error::TYPE_USED_AS_VALUE,
                                                   resolving_.ast.location_of(index.index)));
    }
    if (!index.index.is<ast::range_expr>() && !access_type.is_poison() &&
        !is_integer(access_type.get_kind()) && access_type.get_kind() != type_kind::COMPTIME_INT &&
        !is_generic_type(access_type)) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("An index must be an integer; found '{}'",
                                         ctx_.type_display_name(access_type)),
                             error::TYPE_MISMATCH,
                             resolving_.ast.location_of(index.index)));
    }

    // A subscript range only counts up, unlike a loop's
    if (const auto range{resolving_.ast.get_as_opt<ast::range_expr>(index.index)};
        range && range->lhs && range->rhs) {
        const auto lo_cv{probe_fold(*range->lhs)};
        const auto hi_cv{probe_fold(*range->rhs)};
        const auto lo{lo_cv && !lo_cv->is_poison() ? lo_cv->as_int_opt() : stdx::none};
        const auto hi{hi_cv && !hi_cv->is_poison() ? hi_cv->as_int_opt() : stdx::none};
        const bool inclusive{(*index.index).get_token_type() == syntax::token_type_t::DOT_DOT_EQ};
        if (lo && hi && *lo > *hi + (inclusive ? 1 : 0)) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("A subscript range cannot count down ({} to {}); only `for` loops and "
                            "range values may",
                            static_cast<i64>(*lo),
                            static_cast<i64>(*hi)),
                error::ILLEGAL_OPEN_RANGE,
                resolving_.ast.location_of(index.index)));
        }
    }

    // A constant range over a container of known length is bounds-checked statically
    if (const auto range{resolving_.ast.get_as_opt<ast::range_expr>(index.index)};
        range && range->rhs) {
        if (const auto container_len{known_length(index.array)}) {
            gir::const_eval evaluator{ctx_, resolving_};
            const auto      hi_cv{evaluator.try_eval(*range->rhs)};
            const auto      hi{hi_cv && !hi_cv->is_poison() ? hi_cv->as_int_opt() : stdx::none};
            const bool      inclusive{(*index.index).get_token_type() ==
                                      syntax::token_type_t::DOT_DOT_EQ};
            if (hi && *hi + (inclusive ? 1 : 0) > static_cast<i128>(*container_len)) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     fmt::format("Slice end {} is out of bounds for a length of {}",
                                                 static_cast<i64>(*hi + (inclusive ? 1 : 0)),
                                                 *container_len),
                                     error::SLICE_OUT_OF_BOUNDS,
                                     resolving_.ast.location_of(index.index)));
            }
        }
    }

    // A constant element index into a container of known length is bounds-checked statically
    if (!resolving_.ast.get_as_opt<ast::range_expr>(index.index)) {
        if (const auto container_len{known_length(index.array)}) {
            const auto idx_cv{probe_fold(index.index)};
            const auto idx{idx_cv && !idx_cv->is_poison() ? idx_cv->as_int_opt() : stdx::none};
            // A sentinel-terminated array may read its terminator at index `len`
            const auto arr{concrete_array_type(*target_type).get_data().as_opt<types::array>()};
            const i128 readable{static_cast<i128>(*container_len) +
                                (arr && arr->null_terminated ? 1 : 0)};
            if (idx && (*idx < 0 || *idx >= readable)) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     fmt::format("Index {} is out of bounds for a length of {}",
                                                 boost::int128::to_string(*idx),
                                                 *container_len),
                                     error::SLICE_OUT_OF_BOUNDS,
                                     resolving_.ast.location_of(index.index)));
            }
        }
    }

    // There may be a slice accessor which results in a slice type and should mirror parent
    if (access_type.get_data().is<types::slice>()) {
        // The subslice is writable iff the source container's elements are
        const auto mutability{container_element_mutability(*target_type)};
        last_type_.emplace(ctx_.get_slice(mutability, false, single_item_type));
    } else {
        last_type_.emplace(single_item_type);
    }

    auto& result_type{fold_type_read(
        *target_type, *last_type_.take(), [&](gir::const_eval& ev) { return ev.try_eval(id); })};
    resolving_.set_sema_type(id, result_type);
    last_type_.emplace(result_type);
}

auto type_resolver::names_comptime_mut(ast::expr_handle expr) -> bool {
    const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(expr)};
    if (!ident) { return false; }
    const auto lookup{ctx_.registry.lookup_with_depth(table_stack_, ident->name)};
    if (!lookup) { return false; }
    const auto node{lookup->symbol.get_data().as_opt<symbols::node_t>()};
    if (!node) { return false; }
    const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*node)};
    if (!decl) { return false; }
    return decl->has_modifier(ast::decl_modifiers::COMPTIME) &&
           decl->has_modifier(ast::decl_modifiers::MUT);
}

auto type_resolver::resolve_structural_access(type&                  object_type,
                                              ast::identifier_handle member,
                                              source_location        object_location)
    -> stdx::result<gsl::not_null<type*>, diagnostic> {
    gsl::not_null target_type{&object_type};
    if (const auto ptr_data{target_type->get_data().as_opt<types::pointer>()}) {
        target_type = &ptr_data->underlying;
    } else if (const auto ref_data{target_type->get_data().as_opt<types::reference>()}) {
        target_type = &ref_data->underlying;
    }

    target_type = &concrete_array_type(*target_type);

    auto&      object_data{target_type->get_data()};
    const auto enum_type{object_data.as_opt<types::enum_t>()};
    const auto struct_type{object_data.as_opt<types::struct_t>()};

    // A method reached on an interface-typed receiver resolves against the interface's set.
    if (const auto iface{object_data.as_opt<types::interface_t>()}) {
        const auto& member_ident{resolving_.ast.get_as<ast::identifier_expr>(member)};
        for (usize i{0}; i < iface->method_names.size(); ++i) {
            if (iface->method_names[i] == member_ident.name) { return iface->method_sigs[i]; }
        }
        return make_sema_err(fmt::format("interface has no member named '{}'", member_ident.name),
                             error::UNDECLARED_IDENTIFIER,
                             resolving_.ast.location_of(member));
    }

    // `w.method(...)` on a `&dyn I` resolves against `I`'s method set, with associated types
    // substituted for the `dyn I(Assoc = T)` bindings.
    if (const auto dyn{object_data.as_opt<types::dyn_t>()}) {
        const auto& iface{dyn->interface.get_data().as<types::interface_t>()};
        const auto& member_ident{resolving_.ast.get_as<ast::identifier_expr>(member)};
        for (usize i{0}; i < iface.method_names.size(); ++i) {
            if (iface.method_names[i] != member_ident.name) { continue; }
            if (!iface.method_is_pub[i] && &iface.enclosing != &resolving_) {
                return make_sema_err(
                    fmt::format("`{}` is a sealed interface method and is not callable here",
                                member_ident.name),
                    error::SEALED_METHOD,
                    resolving_.ast.location_of(member));
            }
            auto& sig{resolve_dyn_method_signature(*dyn, iface, i)};
            // The vtable holds one entry per method, but a `mut?` method has one per mutability
            if (const auto fn{sig.get_data().as_opt<types::function>()};
                fn && std::ranges::any_of(fn->params, [](gsl::not_null<const type*> p) {
                    return contains_poly(*p);
                })) {
                return make_sema_err(
                    fmt::format("`{}` takes a `mut?` parameter, so it can't be called through "
                                "`dyn {}`",
                                member_ident.name,
                                ctx_.type_display_name(dyn->interface)),
                    error::ILLEGAL_POLY_MUTABILITY,
                    resolving_.ast.location_of(member));
            }
            return gsl::not_null<type*>{&sig};
        }
        return make_sema_err(fmt::format("`dyn {}` has no method `{}`",
                                         ctx_.type_display_name(dyn->interface),
                                         member_ident.name),
                             error::UNDECLARED_IDENTIFIER,
                             resolving_.ast.location_of(member));
    }
    const auto union_type{object_data.as_opt<types::union_t>()};
    const auto slice_type{object_data.as_opt<types::slice>()};
    const auto array_type{object_data.as_opt<types::array>()};
    const auto closure_type{object_data.as_opt<types::closure_t>()};

    if (closure_type) {
        const auto& member_ident{resolving_.ast.get_as<ast::identifier_expr>(member)};
        if (member_ident.name == "thunk") { return &closure_type->impl_signature; }
        return make_sema_err(fmt::format("Type '{}' has no field named '{}'",
                                         ctx_.type_display_name(*target_type),
                                         member_ident.name),
                             error::UNDECLARED_IDENTIFIER,
                             resolving_.ast.location_of(member));
    }

    if (slice_type || array_type) {
        const auto& member_ident{resolving_.ast.get_as<ast::identifier_expr>(member)};
        auto&       underlying{slice_type ? slice_type->underlying : array_type->underlying};
        if (member_ident.name == "ptr") {
            if (array_type) { structural_members_.insert(ast::node_id{member}.get_index()); }
            return &ctx_.get_pointer(target_type->get_key().get_mut(), underlying);
        }
        if (member_ident.name == "len") {
            if (array_type) { structural_members_.insert(ast::node_id{member}.get_index()); }
            return &ctx_.get_builtin_resolved_type(type_kind::USIZE);
        }
        return make_sema_err(fmt::format("Type '{}' has no field named '{}'",
                                         ctx_.type_display_name(*target_type),
                                         member_ident.name),
                             error::UNDECLARED_IDENTIFIER,
                             resolving_.ast.location_of(member));
    }

    // A type-ctor instance still being built (named by its own member's signature) has no shape
    // yet, only its scope
    const bool pending_instance{!target_type->is_resolved() && target_type->get_instance_id() &&
                                target_type->has_symbol_table_idx()};
    if (!enum_type && !struct_type && !union_type && !pending_instance) {
        const auto& member_ident{resolving_.ast.get_as<ast::identifier_expr>(member)};
        if (auto ext{resolve_impl_method_access(
                *target_type, member_ident.name, resolving_.ast.location_of(member))}) {
            return std::move(*ext);
        }
        return make_sema_err(
            fmt::format(
                "Can only access inner objects inside of structs, unions, and enums; found '{}'",
                ctx_.type_display_name(object_type)),
            error::TYPE_MISMATCH,
            object_location);
    }

    ASSERT(target_type->has_symbol_table_idx(), "Structural should have a resolved table index");
    const auto table_idx{target_type->get_symbol_table_idx()};
    auto&      table{ctx_.registry.get(table_idx)};

    const auto& member_ident{resolving_.ast.get_as<ast::identifier_expr>(member)};
    const scope s{table_stack_, table_idx, table_idx_};
    auto        symbol_proxy{table.get_proxy_opt(member_ident.name)};

    if (!symbol_proxy) {
        if (auto ext{resolve_impl_method_access(
                *target_type, member_ident.name, resolving_.ast.location_of(member))}) {
            return std::move(*ext);
        }
        return make_sema_err(fmt::format("Type '{}' has no field named '{}'",
                                         ctx_.type_display_name(*target_type),
                                         member_ident.name),
                             error::UNDECLARED_IDENTIFIER,
                             resolving_.ast.location_of(member));
    }

    auto& [member_symbol, member_idx] = *symbol_proxy;
    gsl::not_null<type*> result_type  = &ctx_.get_poison();
    if (member_symbol.has_kind() && member_symbol.get_kind() == symbol_kind::POISONED) {
        return result_type;
    }

    if (pending_instance) {
        resolve_symbol(ast::node_id{member}, member_symbol);
        return last_type_.take();
    }
    auto member_type{enum_type     ? enum_type->type_at_opt(member_idx, object_type)
                     : struct_type ? struct_type->type_at_opt(member_idx)
                     : union_type  ? union_type->type_at_opt(member_idx)
                                   : stdx::none};
    ASSERT(enum_type || struct_type || union_type, "Error handling failed to catch invalid type");
    // A member reached from inside its own (still resolving) body goes through its symbol
    if (!member_type) {
        resolve_symbol(ast::node_id{member}, member_symbol);
        return last_type_.take();
    }
    if (enum_type) { return ctx_.pool.strip_volatile(*member_type); }
    return member_type.get();
}

auto type_resolver::get_rightmost_name(ast::expr_handle handle) const noexcept
    -> stdx::option<std::string_view> {
    ast::node_id current{*handle};
    while (true) {
        if (const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(current)}) {
            return ident->name;
        }

        if (const auto dot{resolving_.ast.get_as_opt<ast::dot_expr>(current)}) {
            current = dot->member;
            continue;
        }

        if (const auto deref{resolving_.ast.get_as_opt<ast::dereference_expr>(current)}) {
            current = deref->rhs;
            continue;
        }

        return stdx::none;
    }
}

// `rest.len`: a `usize` known at every call. `.len` is the only member a pack has.
template <ast::IndexableID ID>
auto type_resolver::resolve_pack_len(ID id, const ast::dot_expr& dot) -> void {
    const auto& member{resolving_.ast.get_as<ast::identifier_expr>(dot.member)};
    if (member.name != "len") {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            fmt::format("a parameter pack has only '.len'; found '.{}'", member.name),
            error::PACK_USE_OUT_OF_POSITION,
            resolving_.ast.location_of(dot.member)));
    }
    auto& usize_type{ctx_.get_builtin_resolved_type(type_kind::USIZE)};
    resolving_.set_sema_type(id, usize_type);
    last_type_.emplace(usize_type);
}

template <ast::IndexableID ID>
auto type_resolver::resolve_dot(ID id, const ast::dot_expr& dot) -> void {
    if (const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(dot.object)}) {
        if (pack_named(ident->name)) { return resolve_pack_len(id, dot); }
    }
    resolve(dot.object);
    if (last_type_->is_poison()) { return resolving_.set_sema_type(id, *last_type_); }
    // An ordinary `Color = enum {...}` names its `enum_t` directly, never wrapped
    auto& object_type{denoted_type(*last_type_.take())};

    const auto unwrap_ref = [](type& t) -> type& {
        if (const auto p{t.get_data().as_opt<types::pointer>()}) { return p->underlying; }
        if (const auto r{t.get_data().as_opt<types::reference>()}) { return r->underlying; }
        return t;
    };
    auto&      unwrapped_obj{unwrap_ref(object_type)};
    const bool is_array_or_slice{unwrapped_obj.get_kind() == type_kind::ARRAY ||
                                 unwrapped_obj.get_kind() == type_kind::SLICE ||
                                 unwrapped_obj.get_data().template is<types::deferred_array>()};

    // Desugared to auto and needs to be deferred like the call handler. The `type` keyword
    // itself is no placeholder and has no members
    const bool names_type_keyword{(*dot.object).get_token_type() ==
                                  syntax::token_type_t::TYPE_TYPE};
    if (!is_array_or_slice && !names_type_keyword && is_generic_type(object_type, false)) {
        auto& placeholder{*ctx_.pool[{type_kind::AUTO, types::mut::CONSTANT}]};
        resolving_.set_sema_type(dot.member, placeholder);
        resolving_.set_sema_type(id, placeholder);
        return last_type_.emplace(placeholder);
    }

    if (const auto module{object_type.get_data().as_opt<types::module_t>()}) {
        // The module may not have been resolved yet due to order independence
        auto& inner_mod{module->imported};
        if (inner_mod.is_resolvable()) {
            context new_ctx{ctx_};
            resolve_types(inner_mod, new_ctx);
            if (inner_mod.is_poisoned()) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    fmt::format("Module '{}' failed to resolve due to errors it contains",
                                get_rightmost_name(dot.object).value_or("<module>")),
                    error::IMPORTED_MODULE_CONTAINS_ERRORS,
                    resolving_.ast.location_of(dot.object)));
            }
        }

        // Step into the module's scope for lookup
        const auto& inner_ident{resolving_.ast.get_as<ast::identifier_expr>(dot.member)};
        auto        sym{ctx_.registry.get_from_opt(*inner_mod.root_table_idx, inner_ident.name)};
        if (!sym) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("Module '{}' has no member named '{}'",
                            get_rightmost_name(dot.object).value_or("<expression>"),
                            inner_ident.name),
                error::UNDECLARED_IDENTIFIER,
                resolving_.ast.location_of(dot.member)));
        }

        const auto symbol_node{sym->get_data().as_opt<symbols::node_t>()};
        if (!symbol_node) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }

        if (&inner_mod != &resolving_ && !sym->is_public(inner_mod)) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("Symbol '{}' is private to module '{}'",
                            inner_ident.name,
                            get_rightmost_name(dot.object).value_or("<expression>")),
                error::ILLEGAL_PRIVATE_ACCESS,
                resolving_.ast.location_of(dot.member)));
        }

        stdx::option<ast::type_modifier> mod;
        if constexpr (ast::IndexableExplicitTypeID<ID>) { mod = id.get_modifier(); }
        switch (sym->get_status()) {
        case symbol_status::RESOLVING: {
            const auto poison_out = [&] -> void {
                ctx_.poison_symbol(*sym);
                last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    fmt::format(
                        "Cross-module cyclic dependency detected while resolving symbol '{}'",
                        inner_ident.name),
                    error::CYCLIC_DEPENDENCY,
                    resolving_.ast.location_of(dot.member)));
            };

            // Explicitly reject infinite size cycles across modules before forwarding
            if (!mod || (!mod->is_ptr() && !mod->is_ref())) { return poison_out(); }
            if (const auto forwarded_type{forward_type(inner_mod, mod, *sym)}) {
                resolving_.set_sema_type(dot.member, *forwarded_type);
                resolving_.set_sema_type(id, *forwarded_type);
                return last_type_.emplace(*forwarded_type);
            }

            return poison_out();
        }
        case symbol_status::UNRESOLVED: {
            type_resolver inner_resolver{inner_mod, ctx_};
            inner_resolver.resolve(*symbol_node);
            break;
        }
        case symbol_status::RESOLVED: break;
        }

        if (sym->get_kind_opt() == symbol_kind::POISONED ||
            !inner_mod.has_sema_type(*symbol_node)) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id));
        }

        // Record where this cross-module reference resolves to, for LSP go-to-definition
        resolving_.set_identifier_definition(dot.member,
                                             {inner_mod.path, sym->get_symbol_span(inner_mod)});
        resolving_.add_identifier_position(dot.member);
        record_declaration(ast::node_id{dot.member}, inner_mod, *sym);
        if constexpr (ast::IndexableExplicitTypeID<ID>) { record_declaration(id, inner_mod, *sym); }

        if constexpr (std::same_as<ID, ast::node_id>) {
            record_symbol_owner(id, *inner_mod.root_table_idx, inner_mod, *sym);
        }

        auto& ident_type{inner_mod.get_sema_type(*symbol_node)};
        resolving_.set_sema_type(dot.member, ident_type);
        resolving_.set_sema_type(id, ident_type);
        return last_type_.emplace(ident_type);
    }

    pending_impl_method_owner_.reset();
    pending_param_impl_target_.reset();

    // `Type.field` names no value. A struct field needs an instance (union `U.tag` is a tag)
    if (object_type.get_data().is<types::struct_t>() &&
        operand_nature(dot.object) == operand_nature_t::TYPE) {
        const auto& member_name{resolving_.ast.get_as<ast::identifier_expr>(dot.member).name};
        if (find_aggregate_field(object_type, member_name)) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("'{}' is a field of '{}'; access it through an instance",
                            member_name,
                            ctx_.type_display_name(object_type)),
                error::TYPE_USED_AS_VALUE,
                resolving_.ast.location_of(dot.member)));
        }
    }

    // A slice, array, pointer, or function type has members only through a value of it
    if (const auto kind{object_type.get_kind()};
        (kind == type_kind::SLICE || kind == type_kind::ARRAY || kind == type_kind::POINTER ||
         kind == type_kind::REFERENCE || kind == type_kind::FUNCTION ||
         kind == type_kind::CLOSURE) &&
        operand_nature(dot.object) == operand_nature_t::TYPE) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            fmt::format("Type '{}' has no member named '{}'; its members belong to a value of it",
                        ctx_.type_display_name(object_type),
                        resolving_.ast.get_as<ast::identifier_expr>(dot.member).name),
            error::TYPE_USED_AS_VALUE,
            resolving_.ast.location_of(dot.member)));
    }

    auto result{
        resolve_structural_access(object_type, dot.member, resolving_.ast.location_of(dot.object))};
    if (!result) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(result).error()));
    }

    // A `dyn` method has no value apart from a call through the vtable, and an `impl` method on
    // a primitive has no bound-method form to take either
    const auto& receiver{unwrap_ref(object_type)};
    const auto  method_of_primitive{[&] {
        const auto fn{(*result)->get_data().template as_opt<types::function>()};
        const auto k{receiver.get_kind()};
        return fn && fn->has_self && k != type_kind::STRUCT && k != type_kind::UNION &&
               k != type_kind::ENUM && k != type_kind::TYPE && k != type_kind::MODULE &&
               k != type_kind::CLOSURE;
    }};
    if ((receiver.get_data().template is<types::dyn_t>() || method_of_primitive()) &&
        !callee_dots_.contains(id.get_index())) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            fmt::format("`{}` is a method of this value, so it can only be called",
                        resolving_.ast.get_as<ast::identifier_expr>(dot.member).name),
            error::TYPE_MISMATCH,
            resolving_.ast.location_of(dot.member)));
    }

    // An `impl`-attached method: pin the call target so the emitter names it the same way
    // `emit_top_level_impl` (inherent/trait) or `instantiate_impls_for` (parameterized) does.
    if (pending_impl_method_owner_ || pending_param_impl_target_) {
        if (pending_impl_method_owner_) {
            if constexpr (std::same_as<ID, ast::node_id>) {
                resolving_.set_resolved_symbol_owner(id, *pending_impl_method_owner_);
            }

            // `pending_param_impl_target_` is left set for `resolve_call`
            pending_impl_method_owner_.reset();
        }
        auto& member_type{**result};
        resolving_.set_sema_type(dot.member, member_type);
        resolving_.set_sema_type(id, member_type);
        resolving_.add_identifier_position(dot.member);
        return last_type_.emplace(member_type);
    }

    if (object_type.get_kind() == type_kind::SLICE || object_type.get_kind() == type_kind::ARRAY ||
        unwrap_ref(object_type).get_kind() == type_kind::INTERFACE) {
        auto& member_type{**result};
        resolving_.set_sema_type(dot.member, member_type);
        resolving_.set_sema_type(id, member_type);
        resolving_.add_identifier_position(dot.member);
        return last_type_.emplace(member_type);
    }

    const auto check_access = [&](mod::module_t&   enclosing,
                                  std::string_view type_name,
                                  usize            field_count,
                                  auto&&           is_field_pub) -> bool {
        const auto& table{ctx_.registry.get(object_type.get_symbol_table_idx())};
        const auto& member_ident{resolving_.ast.get_as<ast::identifier_expr>(dot.member)};
        const auto  proxy{table.get_proxy_opt(member_ident.name)};

        // Record where this field/member access resolves to, for LSP go-to-definition
        if (proxy) {
            resolving_.set_identifier_definition(
                dot.member, {enclosing.path, proxy->symbol.get_symbol_span(enclosing)});
            resolving_.add_identifier_position(dot.member);
            record_declaration(ast::node_id{dot.member}, enclosing, proxy->symbol);
            if constexpr (ast::IndexableExplicitTypeID<ID>) {
                record_declaration(id, enclosing, proxy->symbol);
            }
        }

        if (&enclosing == &resolving_) { return true; }
        if (!proxy) { return true; }

        const auto& [member_symbol, member_idx]{*proxy};
        const bool is_field{member_idx < field_count};
        const bool is_pub{is_field ? is_field_pub(member_idx) : member_symbol.is_public(enclosing)};

        if (!is_pub) {
            last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("{} '{}' of {} '{}' is private",
                            is_field ? "Field" : "Member",
                            member_ident.name,
                            type_name,
                            get_rightmost_name(dot.object).value_or("<expression>")),
                error::ILLEGAL_PRIVATE_ACCESS,
                resolving_.ast.location_of(dot.member)));
            return false;
        }
        return true;
    };

    const auto access_ok{object_type.get_data().visit(
        [&](const types::struct_t& s) {
            return check_access(s.enclosing, "struct", s.ast_fields.size(), [&](usize idx) {
                return s.ast_fields[idx].is_public();
            });
        },
        [&](const types::enum_t& e) {
            return check_access(
                e.enclosing, "enum", e.ast_enumerations.size(), [](usize) { return true; });
        },
        [&](const types::union_t& u) {
            return check_access(
                u.enclosing, "union", u.ast_fields.size(), [](usize) { return true; });
        },
        [](const auto&) { return true; })};
    if (!access_ok) { return; }

    // The structural resolver returns poisoned types in error conditions which can be bubbled here
    auto& member_type{fold_type_read(
        object_type, *result.value(), [&](gir::const_eval& ev) -> stdx::option<gir::const_value> {
            const auto object{ev.try_eval(dot.object)};
            const auto st{object ? object->as_opt<gir::const_struct>() : stdx::none};
            if (!st) { return stdx::none; }
            const auto& member_ident{resolving_.ast.get_as<ast::identifier_expr>(dot.member)};
            return st->get_field_opt(member_ident.name).transform([](const auto& f) {
                return gir::const_value{f};
            });
        })};
    if constexpr (std::same_as<ID, ast::node_id>) {
        if (!member_type.is_poison()) { record_member_owner(id, object_type, dot.member); }
    }
    resolving_.set_sema_type(dot.member, member_type);
    resolving_.set_sema_type(id, member_type);
    last_type_.emplace(member_type);
}

VISITOR_TEMPLATE_INIT(type_resolver, resolve_dot, const ast::dot_expr&)

auto type_resolver::visit(ast::node_id id, const ast::dot_expr& dot) -> void {
    PROFILE_FUNCTION();
    resolve_dot(id, dot);
    if (last_type_ && last_type_->is_generic_value()) {
        bind_member_generic_to_receiver(id, dot);
        instantiate_for_expected_callable(id, *last_type_);
    }
}

auto type_resolver::bind_member_generic_to_receiver(ast::node_id id, const ast::dot_expr& dot)
    -> void {
    auto&      generic{*last_type_};
    const auto info{ctx_.generic_functions.get_opt(generic)};
    if (!info || !info->enclosing_type || info->fn_expr->self) { return; }
    const auto receiver{receiver_aggregate(dot)};
    if (!receiver || receiver.get() == info->enclosing_type.get()) { return; }
    const auto prefix{ctx_.generic_functions.get_type_ctor_member_prefix(*receiver)};
    if (!prefix) { return; }

    // Every instance of a type constructor shares the member's literal, so a value taken from
    // one instance names a copy of the generic that remembers which instance it belongs to
    auto& bound{ctx_.local_function_instance(generic, *prefix)};
    if (!bound.is_resolved()) {
        const auto& fn{generic.get_data().as<types::function>()};
        bound.resolve<types::function>(
            fn.params, fn.return_type, fn.has_self, fn.is_variadic, fn.conv);
        ctx_.generic_functions.register_function(bound,
                                                 *info->module,
                                                 info->node_id,
                                                 *info->fn_expr,
                                                 info->name,
                                                 *receiver,
                                                 info->enclosing_fn_table_idx,
                                                 info->enclosing_scopes);
        bound.mark_generic_value();
    }
    resolving_.set_sema_type(id, bound);
    last_type_.emplace(bound);
}

auto type_resolver::resolve_range_value(ast::node_id id, const ast::range_expr& range) -> void {
    // An expected integer array or slice decides the element type
    stdx::option<type&> expected_elem;
    if (const auto expected{implicit_type_stack_.peek()}) {
        const auto& data{concrete_array_type(*expected).get_data()};
        if (const auto arr{data.as_opt<types::array>()}) {
            expected_elem.emplace(arr->underlying);
        } else if (const auto sl{data.as_opt<types::slice>()}) {
            expected_elem.emplace(sl->underlying);
        }
        if (expected_elem && !is_integer(expected_elem->get_kind())) { expected_elem.reset(); }
    }

    {
        const mutating_context_guard subscript_g{in_subscript_index_, false};
        const mutating_context_guard for_iter_g{in_for_iterable_, false};
        const structural_guard       g{implicit_type_stack_, expected_elem};
        TRY_RESOLVE(*range.lhs);
        TRY_RESOLVE(*range.rhs);
    }
    stdx::option<type&> bound_type;
    for (const auto bound : {*range.lhs, *range.rhs}) {
        if (operand_nature(bound) == operand_nature_t::TYPE) {
            return last_type_.emplace(ctx_.poison_node(resolving_,
                                                       id,
                                                       "A range bound must be a value, not a type",
                                                       error::TYPE_USED_AS_VALUE,
                                                       resolving_.ast.location_of(bound)));
        }
        auto& t{resolving_.get_sema_type(bound)};
        if (t.is_poison()) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }
        if (!is_integer(t.get_kind()) && t.get_kind() != type_kind::COMPTIME_INT) {
            return last_type_.emplace(ctx_.poison_node(resolving_,
                                                       id,
                                                       "Range bounds must be integers",
                                                       error::TYPE_MISMATCH,
                                                       resolving_.ast.location_of(id)));
        }
        if (t.get_kind() != type_kind::COMPTIME_INT && !bound_type) { bound_type.emplace(t); }
    }

    // Its length and every element are fixed at compile time
    const auto fold_int{[&](ast::expr_handle bound) -> stdx::option<i128> {
        const auto folded{probe_fold(bound)};
        return folded && !folded->is_poison() ? folded->as_int_opt() : stdx::none;
    }};
    const auto lo{fold_int(*range.lhs)};
    const auto hi{fold_int(*range.rhs)};
    if (!lo || !hi) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            "A range used as a value needs bounds known at compile time; only a `for` loop or a "
            "subscript takes runtime bounds",
            error::ILLEGAL_OPEN_RANGE,
            resolving_.ast.location_of(!lo ? *range.lhs : *range.rhs)));
    }

    // Untyped bounds count in `usize`, or in `isize` once a bound is negative, as a loop does
    auto elem_type{expected_elem};
    if (!elem_type && bound_type) { elem_type.emplace(ctx_.pool.strip_volatile(*bound_type)); }
    if (!elem_type) {
        elem_type.emplace(ctx_.get_builtin_resolved_type(*lo < 0 || *hi < 0 ? type_kind::ISIZE
                                                                            : type_kind::USIZE));
    }
    const auto ptr_bits{target_ptr_bits()};
    for (const auto [bound, value] : {std::pair{*range.lhs, *lo}, std::pair{*range.rhs, *hi}}) {
        if (!comptime_int_fits(value, *elem_type, ptr_bits)) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 fmt::format("Range bound {} does not fit in its element type '{}'",
                                             static_cast<i64>(value),
                                             ctx_.type_display_name(*elem_type)),
                                 error::LITERAL_OUT_OF_RANGE,
                                 resolving_.ast.location_of(bound)));
        }
    }

    const bool inclusive{id.get_token_type() == syntax::token_type_t::DOT_DOT_EQ};
    const auto steps{range_steps_of(*lo, *hi, inclusive)};
    auto&      array_type{
        ctx_.get_array(types::mut::CONSTANT, false, static_cast<usize>(steps.count), *elem_type)};
    resolving_.set_sema_type(id, array_type);
    last_type_.emplace(array_type);
}

auto type_resolver::visit(ast::node_id id, const ast::range_expr& range) -> void {
    PROFILE_FUNCTION();
    auto& usize_type{ctx_.get_builtin_resolved_type(type_kind::USIZE)};

    // Anywhere else a range is the array of values it takes. Match patterns resolve their
    // endpoints directly
    if (range.lhs && range.rhs && !in_subscript_index_ && !in_for_iterable_) {
        return resolve_range_value(id, range);
    }

    // An omitted endpoint is filled from context: the indexed operand inside `[]`, or a sibling
    // iterable of a `for` loop. It is meaningless anywhere else.
    if ((!range.lhs || !range.rhs) && !in_subscript_index_ && !in_for_iterable_) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            "An open-ended range is only valid inside a subscript (`x[lo..]`, `x[..hi]`, `x[..]`) "
            "or a `for` loop paired with an array or slice",
            error::ILLEGAL_OPEN_RANGE,
            resolving_.ast.location_of(id)));
    }
    if (!range.rhs && id.get_token_type() == syntax::token_type_t::DOT_DOT_EQ) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "An inclusive range `..=` requires an upper bound",
                             error::ILLEGAL_OPEN_RANGE,
                             resolving_.ast.location_of(id)));
    }

    // An endpoint is an ordinary value, never itself a range
    const bool                   for_iterable{in_for_iterable_};
    const bool                   unrolled{std::exchange(in_unrolled_for_iterable_, false)};
    const mutating_context_guard subscript_g{in_subscript_index_, false};
    const mutating_context_guard for_iter_g{in_for_iterable_, false};
    gsl::not_null<type*>         lhs_type{&usize_type};
    if (range.lhs) {
        TRY_RESOLVE(*range.lhs);
        lhs_type = last_type_.take();
    }
    if (range.rhs) {
        // Give a bare integer-literal upper bound the lower bound's type (`s.len .. 0`).
        const structural_guard g{implicit_type_stack_, *lhs_type};
        TRY_RESOLVE(*range.rhs);
        auto& rhs_type{*last_type_.take()};

        // ...and give a bare integer-literal lower bound the upper bound's type (`0 .. s.len`).
        if (range.lhs && is_integer(rhs_type.get_kind())) {
            if (const auto lit{resolving_.ast.get_as_opt<ast::int_literal_expr>(*range.lhs)};
                lit && lit->width == 0 && !lit->is_size) {
                resolving_.set_sema_type(*range.lhs, rhs_type);
                lhs_type = &rhs_type;
            }
        }
    }

    for (const auto& bound : {range.lhs, range.rhs}) {
        if (bound && operand_nature(*bound) == operand_nature_t::TYPE) {
            return last_type_.emplace(ctx_.poison_node(resolving_,
                                                       id,
                                                       "A range bound must be a value, not a type",
                                                       error::TYPE_USED_AS_VALUE,
                                                       resolving_.ast.location_of(*bound)));
        }
    }
    const auto is_integral_bound{[](const type& t) {
        return is_integer(t.get_kind()) || t.get_kind() == type_kind::COMPTIME_INT;
    }};
    const auto bound_types_ok{is_integral_bound(*lhs_type) &&
                              (!range.rhs || !resolving_.has_sema_type(*range.rhs) ||
                               is_integral_bound(resolving_.get_sema_type(*range.rhs)))};
    if (!lhs_type->is_poison() && !bound_types_ok) {
        return last_type_.emplace(ctx_.poison_node(resolving_,
                                                   id,
                                                   "Range bounds must be integers",
                                                   error::TYPE_MISMATCH,
                                                   resolving_.ast.location_of(id)));
    }

    // A loop over untyped bounds counts in `usize`, or in `isize` once a bound is negative
    const auto rhs_type{range.rhs ? resolving_.get_sema_type_opt(*range.rhs) : stdx::none};
    if (for_iterable && lhs_type->get_kind() == type_kind::COMPTIME_INT &&
        (!rhs_type || rhs_type->get_kind() == type_kind::COMPTIME_INT)) {
        const auto fold_int{[&](stdx::option<ast::expr_handle> bound) -> stdx::option<i128> {
            if (!bound) { return stdx::none; }
            const auto folded{probe_fold(*bound)};
            return folded ? folded->as_int_opt() : stdx::none;
        }};
        const auto lo{fold_int(range.lhs)};
        const auto hi{fold_int(range.rhs)};
        // `unroll for` unrolls into constants, which keep the default integer type
        const bool negative{(lo && *lo < 0) || (hi && *hi < 0)};
        if (!unrolled) {
            lhs_type =
                &ctx_.get_builtin_resolved_type(negative ? type_kind::ISIZE : type_kind::USIZE);
        }
    }

    // A range over unsuffixed literal bounds (`0..5`) iterates concrete integers, not
    // `comptime_int`.
    lhs_type = &comptime_numeric_view(*lhs_type);

    // Due to deferred type checking just use one endpoint's type for the placeholder slice.
    auto& slice_type{ctx_.get_slice(types::mut::CONSTANT, false, *lhs_type)};
    resolving_.set_sema_type(id, slice_type);
    last_type_.emplace(slice_type);
}

auto type_resolver::visit(ast::node_id id, const ast::implicit_access_expr& implicit_access)
    -> void {
    PROFILE_FUNCTION();
    const auto implicit_type{implicit_type_stack_.peek()};
    // With nothing to name a member of, `.name` is an enum literal of its own
    if (!implicit_type || implicit_type->get_kind() == type_kind::ENUM_LITERAL) {
        auto& literal{ctx_.get_builtin_resolved_type(type_kind::ENUM_LITERAL)};
        resolving_.set_sema_type(implicit_access.member, literal);
        resolving_.set_sema_type(id, literal);
        return last_type_.emplace(literal);
    }

    // `const f: fn(): T = .init;` finds its member through the function's return type
    gsl::not_null member_owner{implicit_type.get()};
    if (const auto fn_data{member_owner->get_data().as_opt<types::function>()}) {
        member_owner = &fn_data->return_type;
    }

    // An `auto` slot takes whatever it's given, so `.name` stays an enum literal there
    if (member_owner->get_kind() == type_kind::AUTO) {
        auto& literal{ctx_.get_builtin_resolved_type(type_kind::ENUM_LITERAL)};
        resolving_.set_sema_type(implicit_access.member, literal);
        resolving_.set_sema_type(id, literal);
        return last_type_.emplace(literal);
    }

    auto result{resolve_structural_access(
        *member_owner, implicit_access.member, resolving_.ast.location_of(id))};
    if (!result) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(result).error()));
    }

    auto& member_type{*result.value()};
    // A struct's field only exists inside a value of it, so `.field` alone names nothing. An
    // initializer's `.field = v` names the field it sets instead
    if (const auto st{member_owner->get_data().as_opt<types::struct_t>()};
        st && !std::exchange(resolving_init_accessor_, false)) {
        const auto& name{resolving_.ast.get_as<ast::identifier_expr>(implicit_access.member).name};
        const auto& table{ctx_.registry.get(member_owner->get_symbol_table_idx())};
        if (const auto proxy{table.get_proxy_opt(name)};
            proxy && proxy->index < st->ast_fields.size()) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("'.{}' is a field of '{}', so it needs a value of it to read from",
                            name,
                            ctx_.type_display_name(*member_owner)),
                error::TYPE_MISMATCH,
                resolving_.ast.location_of(id)));
        }
    }
    if (!member_type.is_poison()) {
        record_member_owner(id, *implicit_type, implicit_access.member);
    }
    resolving_.set_sema_type(implicit_access.member, member_type);
    resolving_.set_sema_type(id, member_type);
    last_type_.emplace(member_type);
}

auto type_resolver::dot_member_symbol(const ast::dot_expr& dot) const
    -> stdx::option<const symbol&> {
    const auto obj_type{resolving_.get_sema_type_opt(dot.object)};
    if (!obj_type) { return stdx::none; }
    const auto& member{resolving_.ast.get_as<ast::identifier_expr>(dot.member)};
    if (const auto mod_data{obj_type->get_data().as_opt<types::module_t>()}) {
        if (!mod_data->imported.root_table_idx) { return stdx::none; }
        const auto sym{ctx_.registry.get_from_opt(*mod_data->imported.root_table_idx, member.name)};
        return sym;
    }
    const auto tbl{denoted_type(*obj_type).get_symbol_table_idx_opt()};
    if (!tbl) { return stdx::none; }
    const auto sym{ctx_.registry.get_from_opt(*tbl, member.name)};
    return sym;
}

} // namespace ghoti::sema
