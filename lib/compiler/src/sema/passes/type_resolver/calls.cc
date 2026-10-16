#include "internal.hh"

#include "compiler/arena.hh"
#include "compiler/ast/format.hh"
#include "compiler/sema/impl_registry.hh"
#include "compiler/sema/instantiation_cache.hh"
#include "support/string_utils.hh"

namespace ghoti::sema {

using namespace resolver_detail;

namespace {

// Why an unmarked parameter is `comptime`: its function reads it at compile time somewhere
[[nodiscard]] auto inferred_comptime_reason(const mod::module_t&                 owner,
                                            const ast::function_expr::parameter& param)
    -> std::string {
    if (!param.comptime_read) {
        return "; the parameter is compile-time because this function reads it in a "
               "compile-time position";
    }
    const auto name{owner.ast.get_as<ast::identifier_expr>(*param.name).name};
    return fmt::format("; '{}' is compile-time because the function reads it at compile time at "
                       "{}:{}{}",
                       name,
                       owner.path.string(),
                       owner.ast.location_of(*param.comptime_read),
                       param.comptime_read_in_const
                           ? ", in a `const` initializer (use `let` to read it at runtime)"
                           : "");
}

// Whether an annotation is `auto` under pointer, reference, slice, and array levels (`&[]auto`)
[[nodiscard]] auto spells_auto_pattern(const ast::AST& tree, stdx::option<ast::explicit_type_id> t)
    -> bool {
    if (!t) { return false; }
    if (t->get_token_type() == syntax::token_type_t::AUTO_TYPE) { return true; }
    switch (t->get_kind()) {
    case ast::explicit_type_kind::RECURSIVE:
        return spells_auto_pattern(tree, tree.get_as<ast::explicit_type_id>(*t));
    case ast::explicit_type_kind::ARRAY:
        return spells_auto_pattern(tree,
                                   tree.get_as<ast::explicit_array_type>(*t).inner_explicit_type);
    default: return false;
    }
}

} // namespace

auto type_resolver::resolve_param_type(stdx::option<ast::explicit_type_id> param_type) -> void {
    const mutating_context_guard g{in_param_type_, true};
    resolve(param_type);
}

auto type_resolver::is_auto_pattern(const type& t) -> bool {
    const auto& data{t.get_data()};
    if (t.get_kind() == type_kind::AUTO) { return true; }
    if (const auto p{data.as_opt<types::pointer>()}) { return is_auto_pattern(p->underlying); }
    if (const auto r{data.as_opt<types::reference>()}) { return is_auto_pattern(r->underlying); }
    if (const auto sl{data.as_opt<types::slice>()}) { return is_auto_pattern(sl->underlying); }
    if (const auto arr{data.as_opt<types::array>()}) { return is_auto_pattern(arr->underlying); }
    if (const auto d{data.as_opt<types::deferred_array>()}) {
        return is_auto_pattern(d->underlying);
    }
    return false;
}

auto type_resolver::match_auto_pattern(type& pattern, type& arg, type*& leaf) -> type* {
    if (pattern.get_kind() == type_kind::AUTO) {
        leaf = &arg;
        return &arg;
    }

    // `mut` needs a mutable argument, a plain level accepts either, and `mut?` takes the argument's
    const auto mutability{
        [&](const type& matched) -> stdx::option<types::mut::mutability_modifiers> {
            if (pattern.is_poly()) {
                return matched.is_constant() ? types::mut::CONSTANT : types::mut::MUTABLE;
            }
            if (!pattern.is_constant() && matched.is_constant()) { return stdx::none; }
            return pattern.is_constant() ? types::mut::CONSTANT : types::mut::MUTABLE;
        }};

    // `[n]auto`'s length folds before its elements can match
    if (pattern.get_data().is<types::deferred_array>()) {
        auto& folded{concrete_array_type(pattern)};
        return &folded != &pattern ? match_auto_pattern(folded, arg, leaf) : nullptr;
    }

    const auto& pdata{pattern.get_data()};
    const auto& adata{arg.get_data()};
    if (const auto p{pdata.as_opt<types::pointer>()}) {
        const auto a{adata.as_opt<types::pointer>()};
        const auto m{a ? mutability(arg) : stdx::none};
        if (!m) { return nullptr; }
        auto* inner{match_auto_pattern(p->underlying, a->underlying, leaf)};
        return inner ? &ctx_.get_pointer(*m, *inner) : nullptr;
    }
    if (const auto r{pdata.as_opt<types::reference>()}) {
        const auto a{adata.as_opt<types::reference>()};
        const auto m{a ? mutability(arg) : stdx::none};
        if (!m) { return nullptr; }
        auto* inner{match_auto_pattern(r->underlying, a->underlying, leaf)};
        return inner ? &ctx_.get_reference(*m, *inner) : nullptr;
    }
    if (const auto sl{pdata.as_opt<types::slice>()}) {
        const auto a{adata.as_opt<types::slice>()};
        const auto m{a ? mutability(arg) : stdx::none};
        if (!m || (sl->null_terminated && !a->null_terminated)) { return nullptr; }
        auto* inner{match_auto_pattern(sl->underlying, a->underlying, leaf)};
        return inner ? &ctx_.get_slice(*m, sl->null_terminated, *inner) : nullptr;
    }
    if (const auto arr{pdata.as_opt<types::array>()}) {
        const auto a{adata.as_opt<types::array>()};
        const auto m{a ? mutability(arg) : stdx::none};
        if (!m || a->len != arr->len || (arr->null_terminated && !a->null_terminated)) {
            return nullptr;
        }
        auto* inner{match_auto_pattern(arr->underlying, a->underlying, leaf)};
        return inner ? &ctx_.get_array(*m, arr->null_terminated, arr->len, *inner) : nullptr;
    }
    return nullptr;
}

auto type_resolver::callable_mismatch(const type& pattern, type& arg) -> stdx::option<std::string> {
    const auto& want{pattern.get_data().as<types::function>()};

    // A function, closure, or erased `fn` value, directly or through `^` / `&`
    type* callee{&denoted_type(arg)};
    if (const auto p{callee->get_data().as_opt<types::pointer>()}) {
        callee = &p->underlying;
    } else if (const auto r{callee->get_data().as_opt<types::reference>()}) {
        callee = &r->underlying;
    }
    stdx::option<const types::function&> have;
    if (const auto cl{callee->get_data().as_opt<types::closure_t>()}) {
        have = cl->signature.get_data().as_opt<types::function>();
    } else {
        have = callee->get_data().as_opt<types::function>();
    }
    if (!have) { return std::string{"it is not callable"}; }
    if (ctx_.generic_functions.get_opt(*callee)) {
        return std::string{"a generic function has no single signature to check"};
    }
    if (have->is_variadic) { return std::string{"it takes C-style variadic arguments"}; }

    const auto skip{have->has_self ? 1UZ : 0UZ};
    const auto have_count{have->params.size() - skip};
    if (have_count != want.params.size()) {
        return fmt::format("it takes {} parameter{}, not {}",
                           have_count,
                           string_utils::plural_suffix(have_count),
                           want.params.size());
    }

    // An `auto` slot deduces from the argument. Anything else must be exactly the same type
    const auto matches{[&](const type& want_t, type& have_t) {
        if (is_auto_pattern(want_t)) {
            type* leaf{nullptr};
            return match_auto_pattern(const_cast<type&>(want_t), have_t, leaf) != nullptr;
        }
        return is_same_unqualified(want_t, have_t);
    }};
    for (usize i{0}; i < want.params.size(); ++i) {
        auto& have_t{*have->params[i + skip]};
        if (!matches(*want.params[i], have_t)) {
            return fmt::format("parameter {} is '{}', not '{}'",
                               i + 1,
                               ctx_.type_display_name(have_t),
                               ctx_.type_display_name(*want.params[i]));
        }
    }
    if (!matches(want.return_type, have->return_type)) {
        return fmt::format("it returns '{}', not '{}'",
                           ctx_.type_display_name(have->return_type),
                           ctx_.type_display_name(want.return_type));
    }
    return stdx::none;
}

auto type_resolver::declared_fn_ref(ast::expr_handle expr) -> stdx::option<gir::const_value> {
    ast::node_id named{expr};
    if (const auto dot{resolving_.ast.get_as_opt<ast::dot_expr>(named)}) { named = dot->member; }
    const auto declared{resolving_.get_identifier_declaration(named)};
    if (!declared || !declared->owner || !declared->decl) { return stdx::none; }
    auto&      owner{const_cast<mod::module_t&>(*declared->owner)};
    const auto decl{owner.ast.get_as_opt<ast::decl_stmt>(*declared->decl)};
    if (!decl || !decl->value || decl->has_modifier(ast::decl_modifiers::MUT) ||
        !owner.ast.get_as_opt<ast::function_expr>(*decl->value)) {
        return stdx::none;
    }
    const auto fn_type{owner.get_sema_type_opt(*decl->value)};
    if (!fn_type || fn_type->get_kind() != type_kind::FUNCTION ||
        ctx_.generic_functions.get_opt(*fn_type)) {
        return stdx::none;
    }
    return gir::const_value{gir::const_value::data_t{
                                gir::const_closure{
                                    .fn_node  = *decl->value,
                                    .module   = owner,
                                    .captures = {},
                                },
                            },
                            *fn_type};
}

auto type_resolver::local_const_fn_ref(ast::expr_handle expr) -> stdx::option<gir::const_value> {
    const auto make_ref{[&](ast::node_id fn_node) -> stdx::option<gir::const_value> {
        const auto fn_type{resolving_.get_sema_type_opt(fn_node)};
        if (!fn_type || fn_type->get_kind() != type_kind::FUNCTION) { return stdx::none; }
        // A capture-less closure value gets emitted as a plain function
        return gir::const_value{gir::const_value::data_t{
                                    gir::const_closure{
                                        .fn_node  = fn_node,
                                        .module   = &resolving_,
                                        .captures = {},
                                    },
                                },
                                *fn_type};
    }};
    // A function literal passed directly
    if (resolving_.ast.get_as_opt<ast::function_expr>(expr)) { return make_ref(expr); }

    const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(expr)};
    if (!ident) { return stdx::none; }
    const auto sym{ctx_.registry.lookup(table_stack_, ident->name)};
    if (!sym) { return stdx::none; }
    const auto node{sym->get_data().as_opt<symbols::node_t>()};
    if (!node) { return stdx::none; }
    const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*node)};
    if (!decl || !decl->value ||
        !(decl->has_modifier(ast::decl_modifiers::LET) ||
          decl->has_modifier(ast::decl_modifiers::COMPTIME))) {
        return stdx::none;
    }
    if (!resolving_.ast.get_as_opt<ast::function_expr>(*decl->value)) { return stdx::none; }
    return make_ref(*decl->value);
}

auto type_resolver::closure_literal_of(ast::expr_handle expr) -> stdx::option<ast::node_id> {
    if (resolving_.ast.get_as_opt<ast::function_expr>(expr)) { return *expr; }

    const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(expr)};
    if (!ident) { return stdx::none; }
    const auto sym{ctx_.registry.lookup(table_stack_, ident->name)};
    if (!sym) { return stdx::none; }
    const auto node{sym->get_data().as_opt<symbols::node_t>()};
    if (!node) { return stdx::none; }
    const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*node)};
    if (!decl || !decl->value ||
        !(decl->has_modifier(ast::decl_modifiers::LET) ||
          decl->has_modifier(ast::decl_modifiers::COMPTIME)) ||
        !resolving_.ast.get_as_opt<ast::function_expr>(*decl->value)) {
        return stdx::none;
    }
    return ast::node_id{*decl->value};
}

auto type_resolver::comptime_closure_value(ast::expr_handle expr)
    -> stdx::option<gir::const_value> {
    const auto fn_node{closure_literal_of(expr)};
    if (!fn_node) { return stdx::none; }

    const auto cl_type{resolving_.get_sema_type_opt(*fn_node)};
    if (!cl_type || cl_type->get_kind() != type_kind::CLOSURE) { return stdx::none; }
    const auto cl_data{cl_type->get_data().as_opt<types::closure_t>()};
    if (!cl_data) { return stdx::none; }

    gir::const_closure result{
        .fn_node  = *fn_node,
        .module   = &resolving_,
        .captures = {},
    };

    for (const auto& capture : cl_data->captures) {
        const auto sym{ctx_.registry.lookup(table_stack_, capture.name)};
        if (!sym) { return stdx::none; }
        const auto node{sym->get_data().as_opt<symbols::node_t>()};
        if (!node) { return stdx::none; }
        const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*node)};
        if (!decl || !decl->value ||
            !(decl->has_modifier(ast::decl_modifiers::LET) ||
              decl->has_modifier(ast::decl_modifiers::COMPTIME))) {
            return stdx::none;
        }
        gir::const_eval evaluator{ctx_, resolving_};
        auto            cv{evaluator.try_eval(*decl->value)};
        if (!cv || cv->is_poison()) { return stdx::none; }
        result.captures.fields.emplace(std::string{capture.name}, std::move(*cv));
    }

    return gir::const_value{gir::const_value::data_t{std::move(result)}, *cl_type};
}

auto type_resolver::reject_type_of_args(const ast::call_expr&     call,
                                        const ast::function_expr& fn_expr,
                                        std::string_view          mangled_name) -> bool {
    // `y: @TypeOf(x)` only has a type in the monomorph, which records it with its request
    const auto request{std::ranges::find(resolving_.generic_instantiations,
                                         mangled_name,
                                         &generic_instantiation_request::mangled_name)};
    if (request == resolving_.generic_instantiations.end()) { return false; }
    for (usize i{0}, runtime_idx{0}; i < fn_expr.parameters.size() && i < call.arguments.size();
         ++i) {
        const auto& param{fn_expr.parameters[i]};
        if (param.is_comptime) { continue; }
        const auto slot{runtime_idx++};
        if (!param.explicit_type ||
            param.explicit_type->get_builtin() != syntax::builtin_t::TYPE_OF ||
            slot >= request->arg_types.size()) {
            continue;
        }
        const auto arg_expr{call.arguments[i].as_opt<ast::expr_handle>()};
        const auto arg_type{arg_expr ? resolving_.get_sema_type_opt(*arg_expr) : stdx::none};
        auto&      param_type{*request->arg_types[slot]};
        if (!arg_type || arg_type->is_poison() || param_type.is_poison() ||
            is_assignable(*arg_type, param_type)) {
            continue;
        }
        ctx_.diags.emplace_back(
            fmt::format("Argument {} of type '{}' is not assignable to parameter type '{}'",
                        i + 1,
                        ctx_.type_display_name(*arg_type),
                        ctx_.type_display_name(param_type)),
            error::TYPE_MISMATCH,
            get_call_arg_location(call.arguments[i]));
        return true;
    }
    return false;
}

template <ast::IndexableID ID>
auto type_resolver::resolve_call(ID id, const ast::call_expr& call) -> void {
    // The call can only yield a non-poison type if the function is valid
    {
        // Only a bare `@builtin(...)` callee is a call of that builtin, not `@builtin.x(...)`
        const mutating_context_guard callee_g{resolving_callee_,
                                              call.function.is<ast::identifier_expr>()};
        const bool dot_callee{call.function.is<ast::dot_expr>() &&
                              callee_dots_.insert(call.function.get_index()).second};
        const auto prev_callee{direct_callee_};
        direct_callee_ =
            call.function.is<ast::dot_expr>()
                ? resolving_.ast.get_as<ast::dot_expr>(call.function).member.get_index()
                : call.function.get_index();
        const auto restore_callee{gsl::finally([&] { direct_callee_ = prev_callee; })};
        resolve(call.function);
        if (dot_callee) { callee_dots_.erase(call.function.get_index()); }
    }
    auto& callee_type{*last_type_.take()};
    if (callee_type.is_poison()) {
        resolve_call_args(call.arguments);
        return last_type_.emplace(ctx_.poison_node(resolving_, id));
    }
    resolving_.set_sema_type(call.function, callee_type);

    // A call whose callee is still generic (e.g. `w.write(...)` where `w: &impl I` desugared to
    // `&auto`) only resolves once the enclosing generic is instantiated.
    if (callee_type.get_kind() == type_kind::AUTO) {
        resolve_call_args(call.arguments);
        resolving_.set_sema_type(id, callee_type);
        return last_type_.emplace(callee_type);
    }

    if constexpr (std::same_as<ID, ast::node_id>) {
        record_type_ctor_member_call(id, call);

        // A `.`-method belonging to a parameterized-impl expansion is emitted under a
        // per-instantiation symbol
        if (pending_param_impl_target_) {
            resolving_.set_generic_call_target(id, std::move(*pending_param_impl_target_));
            pending_param_impl_target_.reset();
        }
    }

    // Verify that the type in the function is callable and store the return type
    auto&                                callee_data{callee_type.get_data()};
    stdx::option<const types::function&> function_type;
    bool                                 is_closure_call{false};
    if (const auto ft{callee_data.as_opt<types::function>()}) {
        function_type = ft;
    } else if (const auto ptr{callee_data.as_opt<types::pointer>()}) {
        function_type = ptr->underlying.get_data().as_opt<types::function>();
    } else if (const auto ref{callee_data.as_opt<types::reference>()}) {
        function_type = ref->underlying.get_data().as_opt<types::function>();
    } else if (const auto cl{callee_data.as_opt<types::closure_t>()}) {
        // Called directly since synthetic `&mut self` param is always implicit here
        function_type   = cl->impl_signature.get_data().as_opt<types::function>();
        is_closure_call = true;
    }

    if (function_type) {
        const auto dot_call{resolving_.ast.get_as_opt<ast::dot_expr>(call.function)};
        bool       is_obj_instance{false};
        if (dot_call) {
            bool       is_type{false};
            const auto target_obj{dot_call->object};
            if (const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(target_obj)}) {
                if (const auto sym{ctx_.registry.lookup(table_stack_, ident->name)}) {
                    if (sym->has_kind() && sym->get_kind() == symbol_kind::TYPE) { is_type = true; }
                }
            } else if (const auto inner_dot{resolving_.ast.get_as_opt<ast::dot_expr>(target_obj)}) {
                if (const auto mod_type{resolving_.get_sema_type_opt(inner_dot->object)}) {
                    if (const auto m_data{mod_type->get_data().as_opt<types::module_t>()}) {
                        const auto& inner_mod{m_data->imported};
                        if (inner_mod.root_table_idx) {
                            const auto& inner_ident{
                                resolving_.ast.get_as<ast::identifier_expr>(inner_dot->member)};
                            if (const auto sym{ctx_.registry.get_from_opt(*inner_mod.root_table_idx,
                                                                          inner_ident.name)}) {
                                if (sym->has_kind() && sym->get_kind() == symbol_kind::TYPE) {
                                    is_type = true;
                                }
                            }
                        }
                    }
                }
            } else if (const auto ot{resolving_.get_sema_type_opt(target_obj)}) {
                if (ot->get_kind() == type_kind::TYPE) { is_type = true; }
            }

            if (!is_type) { is_obj_instance = true; }
        }
        const auto has_implicit_self{is_closure_call ||
                                     (function_type->has_self && is_obj_instance)};

        // Check the arity of the function against params before resetting last type
        const auto& params{function_type->params};
        const usize param_offset{has_implicit_self ? 1UZ : 0UZ};
        // A copy, since resolving the arguments may register generics and move the registry's
        stdx::option<generic_function_info> fn_info_opt;
        if (const auto registered{ctx_.generic_functions.get_opt(callee_type)}) {
            fn_info_opt.emplace(*registered);
        }
        // A pack parameter accepts any number of trailing arguments, like a C variadic.
        const bool has_pack_param{fn_info_opt && !fn_info_opt->fn_expr->parameters.empty() &&
                                  fn_info_opt->fn_expr->parameters.back().is_pack};

        // Splice every `expr...` pack expansion in first, so this call's arity and a pack-taking
        // callee's both see the same flat argument list
        const auto expanded_opt{expand_pack_call_args(call)};
        if (!expanded_opt) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }
        const auto& expanded{*expanded_opt};
        // A monomorph's `mut?` result takes this call's mutability, as a plain function's does
        const auto poly_call_result{[&](type& result) -> type& {
            if (!contains_poly(result)) { return result; }
            return substitute_poly(result,
                                   call_mutability(call,
                                                   *function_type,
                                                   has_implicit_self,
                                                   expanded.source_index,
                                                   expanded.pack_k));
        }};
        const auto effective_arity{expanded.source_index.size()};

        const auto expected_arity{params.size() - param_offset - (has_pack_param ? 1UZ : 0UZ)};
        // `self` written as the first argument (`Type.method(obj, ...)`) is no parameter's default
        const usize explicit_self{function_type->has_self && !has_implicit_self ? 1UZ : 0UZ};
        if constexpr (std::same_as<ID, ast::node_id>) {
            if (effective_arity < expected_arity && effective_arity >= explicit_self &&
                redirect_to_default_wrapper(id, call, effective_arity - explicit_self)) {
                return resolve_call(id, call);
            }
        }
        if (function_type->is_variadic || has_pack_param) {
            if (effective_arity < expected_arity) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     fmt::format("Expected at least {} arguments, found {}",
                                                 expected_arity,
                                                 effective_arity),
                                     error::ARITY_MISMATCH,
                                     resolving_.ast.location_of(call.function)));
            }
        } else if (effective_arity != expected_arity) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("Expected {} arguments, found {}", expected_arity, effective_arity),
                error::ARITY_MISMATCH,
                resolving_.ast.location_of(call.function)));
        }

        if (has_implicit_self && dot_call && is_obj_instance && !function_type->params.empty()) {
            const auto& self_param{*function_type->params[0]};
            const bool  self_requires_mut{!self_param.is_constant() &&
                                          (self_param.get_kind() == type_kind::REFERENCE ||
                                           self_param.get_kind() == type_kind::POINTER)};
            // A method taking `&mut self` or `^mut self` can't be called on a temporary like
            // `(Counter{...}).inc()`, which would mutate a value about to vanish
            if (self_requires_mut && !is_lvalue_expression(resolving_, dot_call->object)) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    "Cannot call method requiring mutable 'self' on a temporary value",
                    error::ILLEGAL_RVALUE_CAPTURE,
                    resolving_.ast.location_of(dot_call->object)));
            }
        }

        if (fn_info_opt && (any_param_generic(params) ||
                            any_param_comptime(*fn_info_opt->fn_expr) || has_pack_param)) {
            auto concrete_arg_types{ctx_.pool.get_many_unsafe(effective_arity)};
            // What `auto` bound to inside a pattern parameter (`x: &auto`), for interface bounds
            std::vector<type*> auto_leaves(effective_arity, nullptr);
            bool               any_arg_poison{false};
            const auto         fixed_params{params.subspan(param_offset)};
            const auto&        fn_params{fn_info_opt->fn_expr->parameters};
            const auto&        fn_ast{fn_info_opt->module->ast};

            const auto find_bound_type_param = [&](usize idx) -> stdx::option<usize> {
                if (idx >= fn_params.size() || !fn_params[idx].explicit_type) { return stdx::none; }
                // `^T` and `&T` keep their modifier in the id, with `T` as the payload
                if (!fn_params[idx].explicit_type->get_modifier().is_value()) { return stdx::none; }
                const auto ident{
                    fn_ast.get_as_opt<ast::identifier_expr>(*fn_params[idx].explicit_type)};
                if (!ident) { return stdx::none; }
                for (usize k{0}; k < idx; ++k) {
                    if (!fn_params[k].name.is<ast::identifier_expr>()) { continue; }
                    if (fn_ast.get_as<ast::identifier_expr>(fn_params[k].name).name ==
                        ident->name) {
                        return k;
                    }
                }
                return stdx::none;
            };

            for (usize i{0}; i < effective_arity; ++i) {
                // Beyond the fixed parameters, every trailing argument is a pack element and
                // types against the pack's own (generic) declared type.
                auto* param_type{i < fixed_params.size() ? fixed_params[i] : fixed_params.back()};

                // A slot an `expr...` expansion produced is already concretely typed (that
                // element's own static type, from the *enclosing* pack) - no AST node to resolve.
                if (expanded.pack_k[i]) {
                    concrete_arg_types[i] = current_pack_->element_types[*expanded.pack_k[i]];
                    continue;
                }

                const auto bound_idx{find_bound_type_param(i)};
                if (bound_idx && concrete_arg_types[*bound_idx]) {
                    param_type = concrete_arg_types[*bound_idx];
                }

                const auto&                    arg{call.arguments[expanded.source_index[i]]};
                stdx::option<structural_guard> g;
                if (!is_generic_type(*param_type)) {
                    g.emplace(implicit_type_stack_, *param_type);
                } else if (const auto arg_h{arg.template as_opt<ast::expr_handle>()};
                           param_type->get_kind() == type_kind::AUTO && arg_h &&
                           resolving_.ast.get_as_opt<ast::implicit_access_expr>(*arg_h)) {
                    // An `auto` slot names nothing, so `.name` there is an enum literal rather
                    // than a member of whatever context surrounds the call
                    g.emplace(implicit_type_stack_,
                              ctx_.get_builtin_resolved_type(type_kind::ENUM_LITERAL));
                }

                auto result_arg_type = arg.visit([this,
                                                  param_type](auto arg_id) -> stdx::option<type&> {
                    if (is_type_parameter_slot(*param_type)) {
                        if (const auto ident{
                                resolving_.ast.get_as_opt<ast::identifier_expr>(arg_id)}) {
                            if (auto sym{
                                    ctx_.registry.lookup_with_table(table_stack_, ident->name)}) {
                                resolving_.set_symbol_table(arg_id, sym->table_idx);
                                if constexpr (std::same_as<decltype(arg_id), ast::node_id>) {
                                    resolving_.add_identifier_position(arg_id);
                                }
                                // A primitive type. An `@builtin` resolves below
                                // and is rejected as uncalled
                                if (auto b{
                                        sym->symbol.get_data().template as_opt<symbols::builtin>()};
                                    b && !ident->name.starts_with('@')) {
                                    return b.value().get_type();
                                }
                                if (auto node{sym->symbol.get_data()
                                                  .template as_opt<symbols::node_t>()}) {
                                    // Only a declaration naming a type. A value
                                    // falls through and is rejected below
                                    if (resolving_.has_sema_type(*node)) {
                                        auto& decl_ty{resolving_.get_sema_type(*node)};
                                        auto& denoted{denoted_type(decl_ty)};
                                        if (decl_ty.get_kind() == type_kind::TYPE &&
                                            denoted.get_kind() != type_kind::TYPE) {
                                            return denoted;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    resolve(arg_id);
                    if (!last_type_) { return stdx::none; }
                    auto* arg_type{last_type_.take()};
                    if (arg_type->is_poison()) { return stdx::none; }
                    // `@TypeOf(x)` in a `type` argument position denotes the type it wraps.
                    if (is_type_parameter_slot(*param_type)) {
                        auto& denoted{denoted_type(*arg_type)};
                        if (denoted.get_kind() != type_kind::TYPE) { return denoted; }
                        if constexpr (std::convertible_to<decltype(arg_id), ast::node_id>) {
                            gir::const_eval evaluator{ctx_, resolving_};
                            if (const auto cv{evaluator.try_eval(ast::node_id{arg_id})};
                                cv && cv->template is<stdx::option<sema::type&>>()) {
                                if (const stdx::option<sema::type&> t{
                                        cv->template as<stdx::option<sema::type&>>()};
                                    t && t->is_resolved()) {
                                    return denoted_type(*t);
                                }
                            }
                        }
                        return denoted;
                    }
                    // Literals materialize and deferred `[n]T` locals fold before binding a generic
                    // parameter, so instantiations stay concrete
                    return &comptime_numeric_view(concrete_array_type(*arg_type));
                });
                // A poisoned argument yields `none`. Record it and move on
                if (!result_arg_type) {
                    concrete_arg_types[i] = nullptr;
                    any_arg_poison        = true;
                    continue;
                }
                auto* resolved_type{result_arg_type.take()};
                // A value slot erases at runtime, so it cannot carry a compile-time-only type
                const auto arg_expr{
                    call.arguments[expanded.source_index[i]].template as_opt<ast::expr_handle>()};
                if (param_type->get_kind() == type_kind::AUTO && arg_expr &&
                    decl_value_denotes_type(*arg_expr)) {
                    return last_type_.emplace(ctx_.poison_node(
                        resolving_,
                        id,
                        fmt::format("Argument {} is the type '{}', but its parameter is an `auto` "
                                    "value; declare the parameter as `T: type` to accept a type",
                                    i,
                                    ctx_.type_display_name(*resolved_type)),
                        error::TYPE_MISMATCH,
                        get_call_arg_location(call.arguments[expanded.source_index[i]])));
                }
                // A slot bound to a concrete value type (`x: T` with `T = u8`) can't take a type
                if (arg_expr && reject_type_as_value(*arg_expr, *param_type)) {
                    return last_type_.emplace(ctx_.poison_node(resolving_, id));
                }
                // An enum literal has no runtime form for a runtime `auto` parameter to hold
                if (resolved_type->get_kind() == type_kind::ENUM_LITERAL &&
                    param_type->get_kind() == type_kind::AUTO && i < fn_params.size() &&
                    !fn_params[i].is_comptime) {
                    return last_type_.emplace(ctx_.poison_node(
                        resolving_,
                        id,
                        fmt::format("Argument {} is an enum literal, which only exists at compile "
                                    "time; make the parameter `comptime`, or pass an enum value",
                                    i + 1),
                        error::TYPE_MISMATCH,
                        get_call_arg_location(call.arguments[expanded.source_index[i]])));
                }
                // ...and a `T: type` slot can't take a value
                const bool declared_type_param{i < fn_params.size() && fn_params[i].spells_type()};
                if (arg_expr && declared_type_param &&
                    operand_nature(*arg_expr) == operand_nature_t::VALUE) {
                    return last_type_.emplace(ctx_.poison_node(
                        resolving_,
                        id,
                        fmt::format("Argument {} is a value of type '{}', but its parameter "
                                    "expects a type",
                                    i,
                                    ctx_.type_display_name(*resolved_type)),
                        error::TYPE_MISMATCH,
                        get_call_arg_location(call.arguments[expanded.source_index[i]])));
                }
                if (bound_idx && concrete_arg_types[*bound_idx] &&
                    !sema::is_assignable(*resolved_type, *param_type)) {
                    return last_type_.emplace(ctx_.poison_node(
                        resolving_,
                        id,
                        fmt::format(
                            "Argument {} of type '{}' is not assignable to parameter "
                            "type '{}' (bound to '{}')",
                            i,
                            ctx_.type_display_name(*resolved_type),
                            ctx_.type_display_name(*param_type),
                            fn_ast.get_as<ast::identifier_expr>(fn_params[*bound_idx].name).name),
                        error::TYPE_MISMATCH,
                        get_call_arg_location(call.arguments[expanded.source_index[i]])));
                }
                // `&auto`, `[]mut auto`, ...: each declared level must match the argument's
                if (!bound_idx && param_type->get_kind() != type_kind::AUTO &&
                    is_auto_pattern(*param_type)) {
                    type*      leaf{nullptr};
                    const auto matched{match_auto_pattern(*param_type, *resolved_type, leaf)};
                    if (!matched) {
                        return last_type_.emplace(ctx_.poison_node(
                            resolving_,
                            id,
                            fmt::format("Argument {} of type '{}' does not match parameter type "
                                        "'{}'",
                                        i + 1,
                                        ctx_.type_display_name(*resolved_type),
                                        ctx_.type_display_name(*param_type)),
                            error::TYPE_MISMATCH,
                            get_call_arg_location(call.arguments[expanded.source_index[i]])));
                    }
                    concrete_arg_types[i] = matched;
                    auto_leaves[i]        = leaf;
                    continue;
                }
                concrete_arg_types[i] =
                    bound_idx && concrete_arg_types[*bound_idx] ? param_type : resolved_type;
            }
            if (any_arg_poison) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }

            // An argument that still contains `auto` (`@TypeOf(a)` where `a: auto`) can only be
            // resolved once the enclosing generic is instantiated
            const auto arg_contains_auto{[](auto&& self, const type& t) -> bool {
                const auto& d{denoted_type(t)};
                if (d.get_kind() == type_kind::AUTO) { return true; }
                const auto& data{d.get_data()};
                if (const auto p{data.template as_opt<types::pointer>()}) {
                    return self(self, p->underlying);
                }
                if (const auto r{data.template as_opt<types::reference>()}) {
                    return self(self, r->underlying);
                }
                if (const auto sl{data.template as_opt<types::slice>()}) {
                    return self(self, sl->underlying);
                }
                if (const auto ar{data.template as_opt<types::array>()}) {
                    return self(self, ar->underlying);
                }
                return false;
            }};
            if (std::ranges::any_of(concrete_arg_types, [&](type* t) {
                    return arg_contains_auto(arg_contains_auto, *t);
                })) {
                auto& placeholder{*ctx_.pool[{type_kind::AUTO, types::mut::CONSTANT}]};
                resolving_.set_sema_type(id, placeholder);
                return last_type_.emplace(placeholder);
            }

            // Enforce `impl I` bounds now the arguments are concrete. A pack's bound covers every
            // trailing argument it collects
            if (const auto it{ctx_.param_bounds.find(fn_info_opt->fn_expr.get())};
                it != ctx_.param_bounds.end()) {
                const auto check_bound_at{[&](usize                        arg_idx,
                                              gsl::span<const type* const> ifaces,
                                              std::string_view pname) -> stdx::option<type&> {
                    gsl::not_null bound_t{&denoted_type(*concrete_arg_types[arg_idx])};
                    if (auto_leaves[arg_idx]) {
                        bound_t = auto_leaves[arg_idx];
                    } else if (const auto p{bound_t->get_data().as_opt<types::pointer>()}) {
                        bound_t = &p->underlying;
                    } else if (const auto r{bound_t->get_data().as_opt<types::reference>()}) {
                        bound_t = &r->underlying;
                    }
                    for (const auto* iface : ifaces) {
                        if (ctx_.impls.implements(*bound_t, *iface)) { continue; }
                        return ctx_.poison_node(
                            resolving_,
                            id,
                            fmt::format("`{}` does not implement `{}` required by parameter `{}`",
                                        ctx_.type_display_name(*bound_t),
                                        ctx_.type_display_name(*iface),
                                        pname),
                            error::UNSATISFIED_BOUND,
                            get_call_arg_location(call.arguments[arg_idx]));
                    }
                    return stdx::none;
                }};
                for (const auto& bound : it->second) {
                    const auto  pidx{static_cast<usize>(bound.param_index)};
                    const auto& ifaces{bound.interfaces};
                    if (pidx >= concrete_arg_types.size()) { continue; }
                    const auto& param{fn_info_opt->fn_expr->parameters[pidx]};
                    // The parameter lives in the callee's own module
                    const auto& pname{
                        fn_info_opt->module->ast.get_as<ast::identifier_expr>(*param.name).name};
                    const auto arg_end{param.is_pack ? concrete_arg_types.size() : pidx + 1};
                    for (usize arg_idx{pidx}; arg_idx < arg_end; ++arg_idx) {
                        if (bound.callable) { continue; }
                        if (auto poison{check_bound_at(arg_idx, ifaces, pname)}) {
                            return last_type_.emplace(*poison);
                        }
                    }
                }
            }

            // Fold each `comptime` parameter's argument under a simulated frame, so it sees earlier
            // `comptime let mut` mutations
            const auto& cx_params{fn_info_opt->fn_expr->parameters};
            const auto  cx_count{static_cast<usize>(
                std::ranges::count_if(cx_params, [](const auto& p) { return p.is_comptime; }))};
            auto        comptime_args{ctx_.arena.make_span<gir::const_value>(cx_count)};
            const comptime_frame_guard sim_guard{ctx_.comptime_binding_frames,
                                                 make_simulated_frame()};
            for (usize i{0}, cx_i{0}; i < cx_params.size() && i < call.arguments.size(); ++i) {
                if (!cx_params[i].is_comptime) { continue; }
                // A `comptime f: fn(...)` is called, so its argument has to be callable
                if (i + param_offset < params.size() && concrete_arg_types[i]) {
                    const auto& param_t{*params[i + param_offset]};
                    const auto  arg_kind{concrete_arg_types[i]->get_kind()};
                    if (param_t.get_kind() == type_kind::FUNCTION &&
                        !concrete_arg_types[i]->is_poison() && arg_kind != type_kind::FUNCTION &&
                        arg_kind != type_kind::CLOSURE && arg_kind != type_kind::POINTER) {
                        return last_type_.emplace(ctx_.poison_node(
                            resolving_,
                            id,
                            fmt::format("Argument {} of type '{}' is not a function, but its "
                                        "parameter is '{}'",
                                        i + 1,
                                        ctx_.type_display_name(*concrete_arg_types[i]),
                                        ctx_.type_display_name(param_t)),
                            error::TYPE_MISMATCH,
                            get_call_arg_location(call.arguments[i])));
                    }
                }
                stdx::option<gir::const_value> folded;
                const auto                     diags_before_eval{ctx_.diags.size()};
                if (const auto expr_h{call.arguments[i].as_opt<ast::expr_handle>()}) {
                    gir::const_eval                               evaluator{ctx_, resolving_};
                    const gir::const_eval::comptime_context_guard g{evaluator, true};
                    if (auto cv{evaluator.try_eval(*expr_h)}; cv && !cv->is_poison()) {
                        folded.emplace(std::move(*cv));
                        // A top-level function folds to its name, which the callee's module can't
                        // look up. Pass the function itself
                        if (folded->is<std::string>()) {
                            if (auto ref{declared_fn_ref(*expr_h)}) { folded = std::move(ref); }
                        }
                    } else if (auto ref{local_const_fn_ref(*expr_h)}) {
                        folded.emplace(std::move(*ref));
                    } else if (auto clv{comptime_closure_value(*expr_h)}) {
                        folded.emplace(std::move(*clv));
                    }
                }

                if (!folded) {
                    if (ctx_.diags.size() > diags_before_eval) {
                        return last_type_.emplace(ctx_.poison_node(resolving_, id));
                    }
                    const auto* arg_ty{concrete_arg_types[i]};
                    auto msg{std::string{arg_ty && arg_ty->get_kind() == type_kind::CLOSURE
                                             ? "a compile-time closure argument must capture only "
                                               "compile-time constants"
                                             : "argument to a comptime parameter must be a "
                                               "compile-time constant"}};
                    if (!cx_params[i].is_comptime_written) {
                        msg += inferred_comptime_reason(*fn_info_opt->module, cx_params[i]);
                    }
                    return last_type_.emplace(
                        ctx_.poison_node(resolving_,
                                         id,
                                         msg,
                                         error::COMPTIME_EVALUATION_FAILED,
                                         get_call_arg_location(call.arguments[i])));
                }
                comptime_args[cx_i++] = std::move(*folded);
            }

            // Copy out of the registry before instantiating: resolving the generic's body may
            // recursively register further generic functions
            generic_function_info fn_info_copy{*fn_info_opt};

            if (dot_call && is_obj_instance && fn_info_copy.enclosing_type) {
                if (const auto recv_t{resolving_.get_sema_type_opt(dot_call->object)}) {
                    auto* target{recv_t.get()};
                    if (const auto p{target->get_data().as_opt<types::pointer>()}) {
                        target = &p->underlying;
                    } else if (const auto r{target->get_data().as_opt<types::reference>()}) {
                        target = &r->underlying;
                    }
                    auto&      denoted{denoted_type(*target)};
                    const auto dk{denoted.get_kind()};
                    if (denoted.is_resolved() &&
                        (dk == type_kind::STRUCT || dk == type_kind::UNION ||
                         dk == type_kind::ENUM)) {
                        fn_info_copy.enclosing_type.emplace(denoted);
                    }
                }
            }

            generic_instantiation_key key{
                .generic_fn_type = &callee_type,
                .arg_types       = concrete_arg_types,
                .comptime_args   = comptime_args,
                .enclosing_type  = fn_info_copy.enclosing_type.raw(),
            };

            if (const auto cached{ctx_.instantiation_cache.find(key)}) {
                if (reject_type_of_args(call, *fn_info_opt->fn_expr, cached->mangled_name)) {
                    return last_type_.emplace(ctx_.poison_node(resolving_, id));
                }
                resolving_.set_generic_call_target(id, cached->mangled_name);
                auto& call_type{poly_call_result(*cached->return_type)};
                resolving_.set_sema_type(id, call_type);
                return last_type_.emplace(call_type);
            }

            const auto                   diags_before_inst{ctx_.diags.size()};
            std::vector<source_location> arg_locations;
            for (const auto& arg : call.arguments) {
                arg_locations.emplace_back(get_call_arg_location(arg));
            }
            const auto prev_locations{
                std::exchange(instantiating_arg_locations_, std::move(arg_locations))};
            auto inst_res{
                instantiate_generic(callee_type, fn_info_copy, concrete_arg_types, comptime_args)};
            instantiating_arg_locations_ = prev_locations;
            const bool reached_compile_error{std::ranges::any_of(
                std::views::iota(diags_before_inst, ctx_.diags.size()), [&](usize d) {
                    return ctx_.diags[d].get_error() == error::COMPILE_ERROR_REACHED;
                })};
            if (reached_compile_error) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     fmt::format("Failed to instantiate '{}'",
                                                 fn_info_copy.name.value_or("<generic function>")),
                                     error::COMPTIME_EVALUATION_FAILED,
                                     resolving_.ast.location_of(call.function)));
            }
            if (!inst_res) {
                // `instantiate_generic` may have already reported so only report if not
                if (ctx_.diags.size() == diags_before_inst) {
                    return last_type_.emplace(ctx_.poison_node(
                        resolving_,
                        id,
                        fmt::format("Failed to instantiate '{}'",
                                    fn_info_copy.name.value_or("<generic function>")),
                        error::COMPTIME_EVALUATION_FAILED,
                        resolving_.ast.location_of(call.function)));
                }
                return last_type_.emplace(ctx_.poison_node(resolving_, id));
            }

            auto& return_type{*inst_res->return_type};
            auto  mangled_name{inst_res->mangled_name};
            if (reject_type_of_args(call, *fn_info_copy.fn_expr, mangled_name)) {
                return last_type_.emplace(ctx_.poison_node(resolving_, id));
            }
            if (!building_param_template_) {
                ctx_.instantiation_cache.insert(std::move(key), return_type, mangled_name);
            }
            resolving_.set_generic_call_target(id, mangled_name);
            auto& call_type{poly_call_result(return_type)};
            resolving_.set_sema_type(id, call_type);
            return last_type_.emplace(call_type);
        }

        bool any_arg_poison{false};
        for (usize i{0}; i < effective_arity; ++i) {
            // A pack-element slot has no AST node of its own to resolve. It aliases an
            // already-typed element of the enclosing pack.
            if (expanded.pack_k[i]) { continue; }

            stdx::option<structural_guard> g;
            if (i < expected_arity) {
                g.emplace(implicit_type_stack_, *function_type->params[i + param_offset]);
            }
            const auto& arg{call.arguments[expanded.source_index[i]]};
            any_arg_poison |= arg.visit([&](auto arg_id) -> bool {
                resolve(arg_id);
                if constexpr (std::same_as<decltype(arg_id), ast::expr_handle>) {
                    if (i < expected_arity && !last_type_->is_poison() &&
                        reject_type_as_value(arg_id, *function_type->params[i + param_offset])) {
                        return true;
                    }
                } else {
                    // A type written in argument position (`f(impl x)`) for a value parameter
                    const auto& param{*function_type->params[i + param_offset]};
                    if (i < expected_arity && !last_type_->is_poison() &&
                        param.get_kind() != type_kind::TYPE && !is_generic_type(param)) {
                        last_type_.emplace(ctx_.poison_node(
                            resolving_,
                            id,
                            fmt::format("Argument {} is a type, but its parameter expects a "
                                        "value of type '{}'",
                                        i + 1,
                                        ctx_.type_display_name(param)),
                            error::TYPE_USED_AS_VALUE,
                            resolving_.ast.location_of(arg_id)));
                        return true;
                    }
                }
                return last_type_.take()->is_poison();
            });
        }
        if (any_arg_poison) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }

        // A `[n]T` return is a (not yet folded) array value, not a type
        const auto returns_deferred_array{
            function_type->return_type.get_data().is<types::deferred_array>()};

        // Functions that return a type cannot be resolved until the constant evaluator
        if (function_type->return_type.get_kind() == type_kind::TYPE && !returns_deferred_array &&
            !any_param_generic(function_type->params)) {
            auto& deferred_type{*ctx_.pool[{type_kind::TYPE, types::mut::CONSTANT, &call}]};
            deferred_type.resolve_if<types::deferred_call>(call);

            resolving_.set_sema_type(id, deferred_type);
            return last_type_.emplace(deferred_type);
        }

        auto* return_type{&function_type->return_type};
        if (returns_deferred_array) { return_type = &concrete_array_type(*return_type); }
        // A `mut?` result takes the call's own mutability, worked out from its arguments
        if (contains_poly(*return_type)) {
            return_type = &substitute_poly(*return_type,
                                           call_mutability(call,
                                                           *function_type,
                                                           has_implicit_self,
                                                           expanded.source_index,
                                                           expanded.pack_k));
        }

        // Only arity is checked since the type checker will handle the rest
        resolving_.set_sema_type(id, *return_type);
        last_type_.emplace(*return_type);
    } else if (const auto builtin_type{callee_data.as_opt<types::builtin_function>()}) {
        // Let `.{...}` infer its fields without spelling out `builtin.XInfo{...}`.
        stdx::option<type&> descriptor_hint;
        if (call.function->get_builtin()) {
            switch (*call.function->get_builtin()) {
            case syntax::builtin_t::INT:
                descriptor_hint.emplace(ctx_.get_builtin_type("IntInfo"));
                break;
            case syntax::builtin_t::FLOAT:
                descriptor_hint.emplace(ctx_.get_builtin_type("FloatInfo"));
                break;
            case syntax::builtin_t::POINTER:
            case syntax::builtin_t::REFERENCE:
                descriptor_hint.emplace(ctx_.get_builtin_type("PointerInfo"));
                break;
            case syntax::builtin_t::SLICE:
                descriptor_hint.emplace(ctx_.get_builtin_type("SliceInfo"));
                break;
            case syntax::builtin_t::ARRAY:
                descriptor_hint.emplace(ctx_.get_builtin_type("ArrayInfo"));
                break;
            case syntax::builtin_t::FN:
                descriptor_hint.emplace(ctx_.get_builtin_type("FnInfo"));
                break;
            case syntax::builtin_t::STRUCT:
                descriptor_hint.emplace(ctx_.get_builtin_type("StructInfo"));
                break;
            case syntax::builtin_t::UNION:
                descriptor_hint.emplace(ctx_.get_builtin_type("UnionInfo"));
                break;
            case syntax::builtin_t::ENUM:
                descriptor_hint.emplace(ctx_.get_builtin_type("EnumInfo"));
                break;
            case syntax::builtin_t::BRANCH_HINT:
                descriptor_hint.emplace(ctx_.get_builtin_type("BranchHint"));
                break;
            default: break;
            }
        }

        auto args_result{resolve_result::OK};
        if (call.function->get_builtin() == syntax::builtin_t::EXPORT &&
            call.arguments.size() == 2) {
            // The function stands alone. The options are a `builtin.ExportOptions` literal
            {
                const structural_guard shield{implicit_type_stack_, nullptr};
                args_result = resolve_call_args(
                    gsl::span<const ast::call_expr::argument>{call.arguments}.first(1));
            }
            if (args_result == resolve_result::OK) {
                const structural_guard g{implicit_type_stack_,
                                         ctx_.get_builtin_type("ExportOptions")};
                args_result = resolve_call_args(
                    gsl::span<const ast::call_expr::argument>{call.arguments}.subspan(1));
            }
        } else if (descriptor_hint && !call.arguments.empty()) {
            const structural_guard g{implicit_type_stack_, *descriptor_hint};
            args_result = call.arguments[0].visit([this](auto arg_id) {
                resolve(arg_id);
                return last_type_.take()->is_poison() ? resolve_result::POISONED
                                                      : resolve_result::OK;
            });
            if (args_result == resolve_result::OK && call.arguments.size() > 1) {
                args_result = resolve_call_args(
                    gsl::span<const ast::call_expr::argument>{call.arguments}.subspan(1));
            }
        } else if (call.function->get_builtin() == syntax::builtin_t::TYPE_OF ||
                   call.function->get_builtin() == syntax::builtin_t::INT_FROM_FLOAT ||
                   call.function->get_builtin() == syntax::builtin_t::FLOAT_FROM_INT ||
                   call.function->get_builtin() == syntax::builtin_t::FLOAT_CAST ||
                   (call.function->get_builtin() &&
                    gir::semantics::math_function_of(*call.function->get_builtin()))) {
            // The result type must not flow into a literal operand of the other numeric kind
            const structural_guard shield{implicit_type_stack_, nullptr};
            args_result = resolve_call_args(call.arguments);
        } else if (const auto tok{call.function->get_builtin()};
                   (tok == syntax::builtin_t::AS || tok == syntax::builtin_t::BIT_CAST ||
                    tok == syntax::builtin_t::INT_CAST || tok == syntax::builtin_t::TRUNCATE) &&
                   call.arguments.size() == 2) {
            const auto is_numeric_literal = [this](const ast::call_expr::argument& arg) {
                const auto handle{arg.as_opt<ast::expr_handle>()};
                if (!handle) { return false; }
                ast::node_id node{*handle};
                if (const auto un{resolving_.ast.get_as_opt<ast::unary_expr>(node)};
                    un && node.get_token_type() == syntax::token_type_t::MINUS) {
                    node = un->rhs;
                }
                return resolving_.ast[node].is<ast::int_literal_expr>() ||
                       resolving_.ast[node].is<ast::float_literal_expr>();
            };
            {
                const structural_guard shield{implicit_type_stack_, nullptr};
                args_result = resolve_call_args(
                    gsl::span<const ast::call_expr::argument>{call.arguments}.first(1));
            }
            // A cast's operand is typed by the cast, never by the context the cast sits in
            const bool as_literal{tok == syntax::builtin_t::AS &&
                                  is_numeric_literal(call.arguments[1])};
            if (args_result == resolve_result::OK && as_literal) {
                // The literal is the cast target's, not the surrounding context's
                stdx::option<type&> literal_hint;
                ast::node_id        literal{*call.arguments[1].as_opt<ast::expr_handle>()};
                if (const auto negated{resolving_.ast.get_as_opt<ast::unary_expr>(literal)}) {
                    literal = negated->rhs;
                }
                if (resolving_.ast[literal].is<ast::float_literal_expr>() &&
                    call_arg_denotes_type(call.arguments[0])) {
                    auto& target{denoted_type(*get_resolved_call_arg_type(call.arguments[0]))};
                    if (is_float(target.get_kind())) { literal_hint.emplace(target); }
                }
                const auto             outer_hint{implicit_type_stack_.peek()};
                const structural_guard g{implicit_type_stack_,
                                         literal_hint ? literal_hint.get() : outer_hint.raw()};
                args_result = resolve_call_args(
                    gsl::span<const ast::call_expr::argument>{call.arguments}.subspan(1));
            } else if (args_result == resolve_result::OK) {
                stdx::option<type&> operand_hint;
                if (tok == syntax::builtin_t::AS && call_arg_denotes_type(call.arguments[0])) {
                    operand_hint.emplace(
                        denoted_type(*get_resolved_call_arg_type(call.arguments[0])));
                }
                const structural_guard g{implicit_type_stack_, operand_hint.raw()};
                args_result = resolve_call_args(
                    gsl::span<const ast::call_expr::argument>{call.arguments}.subspan(1));
            }
        } else if ((tok == syntax::builtin_t::INT_CAST || tok == syntax::builtin_t::BIT_CAST ||
                    tok == syntax::builtin_t::TRUNCATE || tok == syntax::builtin_t::INT_FROM_BOOL ||
                    tok == syntax::builtin_t::FROM_BACKING_INT) &&
                   call.arguments.size() == 1) {
            // The inferred target is the cast's result, not its operand's type
            const structural_guard shield{implicit_type_stack_, nullptr};
            args_result = resolve_call_args(call.arguments);
        } else if (!atomic_arg_roles(call.function->get_builtin()).empty()) {
            args_result = resolve_atomic_call_args(call);
        } else {
            args_result = resolve_call_args(call.arguments);
        }

        // There's no need to check any further if the arguments are poisoned
        if (args_result == resolve_result::POISONED) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id));
        }

        // Poison the call if there's an error early
        auto result{resolve_builtin_call(id, call, *builtin_type)};
        if (!result) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(result.error())));
        }
    } else {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("Expression of type '{}' is not callable",
                                         ctx_.type_display_name(callee_type)),
                             error::NON_CALLABLE_EXPRESSION,
                             resolving_.ast.location_of(call.function)));
    }
}

VISITOR_TEMPLATE_INIT(type_resolver, resolve_call, const ast::call_expr&)

auto type_resolver::visit(ast::node_id id, const ast::call_expr& call) -> void {
    PROFILE_FUNCTION();
    if (call.modifier) {
        if (call.function->get_token_type() == syntax::token_type_t::BUILTIN) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 "'@call' calls a function; a builtin isn't one",
                                 error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(call.function)));
        }
        if (!resolve_call_modifier(id, *call.modifier)) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id));
        }
    }
    resolve_call(id, call);
}

// `@call`'s modifier must be a compile-time `builtin.CallModifier`
auto type_resolver::resolve_call_modifier(ast::node_id id, ast::expr_handle modifier) -> bool {
    auto& modifier_type{ctx_.get_builtin_type("CallModifier")};
    {
        const structural_guard g{implicit_type_stack_, modifier_type};
        resolve(modifier);
    }
    const auto resolved{last_type_.take()};
    if (resolved->is_poison()) { return false; }
    const auto location{resolving_.ast.location_of(*modifier)};
    if (!is_assignable(*resolved, modifier_type)) {
        ctx_.diags.emplace_back(fmt::format("'@call' expects a builtin.CallModifier, found '{}'",
                                            ctx_.type_display_name(*resolved)),
                                error::TYPE_MISMATCH,
                                location);
        return false;
    }

    gir::const_eval evaluator{ctx_, resolving_};
    const auto      folded{evaluator.try_eval(modifier)};
    const auto      tag{folded ? folded->as_opt<gir::const_enum>() : stdx::none};
    const auto      parsed{tag ? ast::call_modifier_from_name(tag->name) : stdx::none};
    if (!parsed) {
        ctx_.diags.emplace_back("'@call' modifier must be known at compile time",
                                error::COMPTIME_EVALUATION_FAILED,
                                location);
        return false;
    }
    resolving_.call_modifiers.insert_or_assign(id.get_index(), *parsed);
    return true;
}

auto type_resolver::visit(ast::node_id id, const ast::function_expr& fn) -> void {
    PROFILE_FUNCTION();

    const bool has_pack{
        std::ranges::any_of(fn.parameters, [](const auto& p) { return p.is_pack; })};

    if (has_pack && fn.is_type_expr) {
        return last_type_.emplace(ctx_.poison_node(resolving_,
                                                   id,
                                                   "a parameter pack cannot appear in a `fn(...): "
                                                   "type` value expression",
                                                   error::MALFORMED_PACK_USE,
                                                   resolving_.ast.location_of(id)));
    }

    // Interface requirements resolve elsewhere. Anywhere else a method must have a body
    if (fn.is_type_expr && fn.self) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "A method needs a body; only interface methods may omit it",
                             error::FUNCTION_DECLARATION_MISSING_BODY,
                             resolving_.ast.location_of(id)));
    }

    // Each instantiation fixes its arity, which leaves nothing for C-style varargs to carry
    const bool is_generic{declares_generic_params(fn)};
    if (is_generic && fn.variadic) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "A generic function cannot take C-style variadic arguments (`...`); "
                             "use a parameter pack (`rest...`) instead",
                             error::MALFORMED_PACK_USE,
                             resolving_.ast.location_of(id)));
    }

    resolve_fn_literal_attributes(id, fn, is_generic);

    if (!target_supports_callconv(fn.conv)) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("`callconv(.{})` is not supported on the current target",
                                         ast::calling_convention_name(fn.conv)),
                             error::TYPE_MISMATCH,
                             resolving_.ast.location_of(id)));
    }

    if (fn.is_type_expr && !fn.self && !fn.is_extern && fn.has_explicit_conv) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_, id, callconv_requires_extern(resolving_.ast.location_of(id))));
    }

    if (fn.is_type_expr) {
        const auto true_param_count{fn.parameters.size() + (fn.self ? 1UZ : 0UZ)};
        auto       fn_param_types{ctx_.pool.get_many_unsafe(true_param_count)};
        usize      p_idx{0};
        if (fn.self) {
            fn_param_types[p_idx++] = &ctx_.get_builtin_resolved_type(type_kind::OPAQUE);
        }
        for (const auto& param : fn.parameters) {
            resolve_param_type(param.explicit_type);
            if (!last_type_) { return; }
            auto& param_type{denoted_type(*last_type_.take())};
            if (reject_non_runtime_slot(param.explicit_type, param_type)) {
                return last_type_.emplace(ctx_.poison_node(resolving_, id));
            }
            fn_param_types[p_idx++] = &param_type;
        }
        {
            const mutating_context_guard ret_g{in_return_type_, true};
            TRY_RESOLVE(fn.explicit_return_type);
        }
        auto& return_type{denoted_type(*last_type_.take())};
        if (reject_non_runtime_slot(fn.explicit_return_type, return_type)) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id));
        }

        auto& fn_type{[&] -> type& {
            if (!fn.self) {
                return ctx_.get_function(
                    fn_param_types, return_type, fn.variadic, fn.conv, !fn.is_extern);
            }
            types::key_t fn_key{type_kind::FUNCTION, types::mut::CONSTANT};
            for (const auto* p : fn_param_types) { fn_key.imprint(*p); }
            fn_key.imprint(return_type);
            fn_key.imprint(fn.conv);
            auto& self_fn{*ctx_.pool[fn_key]};
            self_fn.resolve_if<types::function>(
                fn_param_types, return_type, true, fn.variadic, fn.conv);
            return self_fn;
        }()};

        auto& meta{*ctx_.pool[{type_kind::TYPE, types::mut::CONSTANT, fn_type}]};
        meta.resolve_if<types::meta_type>(fn_type);
        resolving_.set_sema_type(id, meta);
        return last_type_.emplace(meta);
    }

    // The entire function lives inside of its preallocated scope
    auto& fn_type{resolving_.get_sema_type(id)};

    if (for_generic_instantiation_ && reresolve_floor_ && fn_type.has_symbol_table_idx() &&
        fn_type.get_symbol_table_idx() >= *reresolve_floor_) {
        fn_type.unresolve();
    }

    // `pre_register_impls` force-drives an impl target aggregate's decl. The
    // signature is already committed, so a re-entry is a no-op rather than an assertion failure.
    if (fn_type.is_resolved()) {
        resolving_.set_sema_type(id, fn_type);
        return last_type_.emplace(fn_type);
    }

    const scope s{table_stack_, fn_type.get_symbol_table_idx(), table_idx_};

    const function_boundary_guard fn_boundary{function_boundaries_, table_stack_.size() - 1};
    const open_function_guard     fn_node{open_function_nodes_, id};
    const self_recursion_guard    fn_self_ref{self_recursive_flags_, false};

    const auto true_param_size{fn.parameters.size() + (fn.self ? 1 : 0)};
    auto       param_types{ctx_.pool.get_many_unsafe(true_param_size)};
    usize      param_idx{0};

    if (fn.self) {
        // If self is valid here, then follow a similar tune to the type resolvers
        if (const auto user_type{user_type_stack_.peek()}) {
            const auto modifier{fn.self->modifier};
            const auto mutability{types::mut::from_type_modifier(modifier)};

            if (user_type->is_poison()) {
                return last_type_.emplace(ctx_.poison_node(resolving_, id));
            }

            if (!mutability) {
                last_type_.emplace(*user_type);
                resolving_.set_sema_type(fn.self->name, *last_type_);
            } else {
                // Imprint generally here since a new type is always created (else unreachable)
                auto new_key{user_type->get_key()};
                new_key.set_mut(*mutability);
                new_key.imprint(*user_type);

                if (modifier.is_ptr()) {
                    new_key.set_kind(type_kind::POINTER);
                    last_type_.emplace(ctx_.pool[new_key]);
                    last_type_->resolve_if<types::pointer>(*user_type);
                    resolving_.set_sema_type(fn.self->name, *last_type_);
                } else if (modifier.is_ref()) {
                    new_key.set_kind(type_kind::REFERENCE);
                    last_type_.emplace(ctx_.pool[new_key]);
                    last_type_->resolve_if<types::reference>(*user_type);
                    resolving_.set_sema_type(fn.self->name, *last_type_);
                } else {
                    UNREACHABLE("Self parameter has a modifier that slipped through the parser");
                }
            }
        } else {
            resolve_symbol_info(fn.self->name, symbol_kind::POISONED);
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 "Self parameters may only be used inside member functions",
                                 error::ILLEGAL_SELF_PARAMETER,
                                 resolving_.ast.location_of(fn.self->name)));
        }

        param_types[param_idx++] = last_type_.take();
        resolve_symbol_info(fn.self->name, symbol_kind::VALUE);
    }

    // Every parameter contributes to the resolution but not the type key due to unique idx
    for (const auto& param : fn.parameters) {
        if (param.name.is<ast::identifier_expr>()) {
            const auto& ident{resolving_.ast.get_as<ast::identifier_expr>(param.name)};
            if (auto sym{ctx_.registry.get_from_opt(table_idx_, ident.name)}) {
                sym->set_status(symbol_status::RESOLVING);
            }
        }
        // An untyped pack (`rest...`) has no type to resolve. It's implicitly generic, like `auto`.
        if (param.is_pack && !param.explicit_type) {
            last_type_.emplace(ctx_.pool[{type_kind::AUTO, types::mut::CONSTANT}]);
        } else {
            resolve_param_type(param.explicit_type);
            if (!last_type_) { return; }
        }

        auto& param_type{thin_if_comptime(param, denoted_type(*last_type_.take()))};
        if (reject_non_runtime_slot(param.explicit_type, param_type)) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id));
        }
        // A `type` is only ever known at compile time, and saying so keeps every compile-time
        // parameter spelled the same way
        if (!param.is_comptime_written && param_type.get_kind() == type_kind::TYPE &&
            param.spells_type()) {
            ctx_.diags.emplace_back(
                fmt::format("A parameter of type 'type' must be written 'comptime {}: type'",
                            ast::parameter_name(resolving_.ast, param)),
                error::TYPE_PARAM_NOT_COMPTIME,
                resolving_.ast.location_of(param.name));
        }
        param_types[param_idx++] = &param_type;
        resolving_.set_sema_type(param.name, param_type);
        if (param.name.is<ast::identifier_expr>() && param.explicit_type) {
            resolving_.set_identifier_declaration(
                param.name,
                declaration_ref{
                    .owner = &resolving_, .decl = stdx::none, .annotation = param.explicit_type});
        }
        if (param.name.is<ast::identifier_expr>()) {
            resolve_symbol_info(param.name, symbol_kind::VALUE);
        }
    }

    {
        const mutating_context_guard ret_g{in_return_type_, true};
        TRY_RESOLVE(fn.explicit_return_type);
    }
    auto& return_type{denoted_type(*last_type_.take())};
    if (reject_non_runtime_slot(fn.explicit_return_type, return_type)) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id));
    }
    // A `mut?` result is the call's mutability, which only `mut?` parameters can supply
    if (spells_poly(resolving_.ast, fn.explicit_return_type) &&
        std::ranges::none_of(param_types, [](const type* p) { return contains_poly(*p); })) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "A `mut?` result needs a `mut?` parameter to take its mutability from",
                             error::ILLEGAL_POLY_MUTABILITY,
                             resolving_.ast.location_of(fn.explicit_return_type)));
    }
    // A type-ctor member whose signature instantiates its own ctor (`Result(U, E)` in `Result`)
    // gets this node typed by that nested instance, so this instance's signature replaces it
    const auto self_offset{fn.self.has_value() ? 1UZ : 0UZ};
    const bool any_param_needs_own_instantiation{[&] {
        if (self_offset && is_generic_type(*param_types[0])) { return true; }
        for (usize i{self_offset}; i < param_types.size(); ++i) {
            const auto& pt{*param_types[i]};
            if (is_auto_pattern(pt)) { return true; }
            if (pt.get_kind() == type_kind::TYPE) {
                if (fn.parameters[i - self_offset].spells_type()) { return true; }
                continue;
            }
            if (is_generic_type(pt)) { return true; }
        }
        return false;
    }()};

    // A `comptime` parameter makes the function a template, monomorphized per value
    if (any_param_needs_own_instantiation || any_param_comptime(fn)) {
        fn_type.resolve<types::function>(
            param_types, return_type, fn.self.has_value(), fn.variadic, fn.conv);
        // Only a function directly at the impl/aggregate level is a genuine method whose
        // instantiation should see the enclosing type's scope/self-binding
        const auto enclosing_for_generic{function_boundaries_.size() <= 1 ? user_type_stack_.peek()
                                                                          : stdx::none};

        stdx::option<usize> enclosing_fn_table_idx;
        if (enclosing_for_generic && enclosing_for_generic->has_symbol_table_idx()) {
            const auto          agg_table_idx{enclosing_for_generic->get_symbol_table_idx()};
            stdx::option<usize> preceding;
            for (const auto idx : table_stack_) {
                if (idx == agg_table_idx) {
                    enclosing_fn_table_idx = preceding;
                    break;
                }
                preceding.emplace(idx);
            }
        }
        ctx_.generic_functions.register_function(
            fn_type, resolving_, id, fn, stdx::none, enclosing_for_generic, enclosing_fn_table_idx);
        register_impl_param_bounds(fn);
        return last_type_.emplace(fn_type);
    }

    const auto is_auto_return{return_type.get_kind() == type_kind::AUTO};

    // A known return type lets a recursive call inside the body resolve against this signature
    if (!is_auto_return) {
        fn_type.resolve<types::function>(
            param_types, return_type, fn.self.has_value(), fn.variadic, fn.conv);
    }

    return_trackers_.emplace_back(return_tracker{
        .return_types   = {},
        .is_auto_return = is_auto_return,
        .expected_type  = is_auto_return ? stdx::none : stdx::option<type&>{return_type},
    });

    const auto& block{resolving_.ast.get_as<ast::block_stmt>(*fn.body)};
    const bool  body_failed{resolve_block_statements(block)};
    auto        tracker{std::move(return_trackers_.back())};
    return_trackers_.pop_back();
    if (body_failed) {
        // A template's explicit signature stands alone. Each instantiation re-resolves the body
        if (building_param_template_ && !is_auto_return) { return last_type_.emplace(fn_type); }
        return resolving_.set_sema_type(id, *last_type_);
    }

    // A compile-time-only result never reaches the runtime check for a missing `return`
    const auto ret_kind{return_type.get_kind()};
    if (!is_auto_return && !tracker.has_returns() &&
        (ret_kind == type_kind::COMPTIME_INT || ret_kind == type_kind::COMPTIME_FLOAT ||
         ret_kind == type_kind::TYPE)) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("Function returning '{}' never returns a value",
                                         ctx_.type_display_name(return_type)),
                             error::RETURN_TYPE_MISMATCH,
                             resolving_.ast.location_of(id)));
    }

    auto& deduced_return_type{is_auto_return ? tracker.deduced_return_type(ctx_) : return_type};
    if (is_auto_return) {
        fn_type.resolve<types::function>(
            param_types, deduced_return_type, fn.self.has_value(), fn.variadic, fn.conv);
        resolving_.set_sema_type(fn.explicit_return_type, deduced_return_type);
    }

    const auto has_captures{!resolving_.get_captures(id).empty()};
    if (self_recursive_flags_.back() && has_captures) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "A closure cannot call itself by name; use @fnCtx() instead",
                             error::ILLEGAL_RECURSIVE_CLOSURE,
                             resolving_.ast.location_of(id)));
    }

    if (has_captures) { return last_type_.emplace(attach_closure_type(id, fn_type, fn.is_move)); }
    last_type_.emplace(fn_type);
}

auto type_resolver::infer_capture_mode(capture_usage usage,
                                       const type&   captured_type,
                                       bool          force_move) noexcept -> types::capture_mode {
    // `move fn` copies every capture into the environment, even a mutated one
    if (force_move) { return types::capture_mode::VALUE; }
    if (usage == capture_usage::MUTATED) { return types::capture_mode::MUT_REF; }

    // A captured local function may have no storage of its own to reference
    const auto kind{captured_type.get_kind()};
    const auto by_value{is_numeric(kind) || kind == type_kind::BOOL || kind == type_kind::POINTER ||
                        kind == type_kind::FUNCTION || kind == type_kind::CLOSURE};
    return by_value ? types::capture_mode::VALUE : types::capture_mode::REF;
}

auto type_resolver::attach_closure_type(ast::node_id fn_id, type& fn_type, bool is_move) -> type& {
    const auto captures{resolving_.get_captures(fn_id)};
    ASSERT(!captures.empty(), "attach_closure_type called with no captures");

    const auto idx{fn_type.get_symbol_table_idx()};
    auto*      closure_type{ctx_.pool[{type_kind::CLOSURE, types::mut::CONSTANT, idx}].get()};
    closure_type->set_symbol_table_idx(idx);

    auto  capture_span{ctx_.arena.make_span<types::closure_capture>(captures.size())};
    usize i{0};
    for (const auto& capture : captures) {
        auto lookup{ctx_.registry.lookup(table_stack_, capture.name)};
        ASSERT(lookup, "Recorded capture name failed to re-resolve in its enclosing scope");
        auto&      captured_type{get_resolved_symbol_type(lookup->get_data())};
        const auto mode{infer_capture_mode(capture.usage, captured_type, is_move)};

        // What the environment actually stores: the value itself, or a reference to it
        auto* storage_type{&captured_type};
        if (mode == types::capture_mode::REF) {
            storage_type = &ctx_.get_reference(types::mut::CONSTANT, captured_type);
        } else if (mode == types::capture_mode::MUT_REF) {
            storage_type = &ctx_.get_reference(types::mut::MUTABLE, captured_type);
        }

        capture_span[i++] =
            types::closure_capture{capture.name, &captured_type, storage_type, mode};
    }

    // Inject mutable self reference into closure call arguments
    const auto& public_sig{fn_type.get_data().as<types::function>()};
    auto        impl_params{ctx_.pool.get_many_unsafe(public_sig.params.size() + 1)};
    impl_params[0] = &ctx_.get_reference(types::mut::MUTABLE, *closure_type);
    for (usize p{0}; p < public_sig.params.size(); ++p) {
        impl_params[p + 1] = public_sig.params[p];
    }

    auto* impl_sig{ctx_.pool[{type_kind::FUNCTION, types::mut::CONSTANT, idx, true}].get()};
    impl_sig->resolve<types::function>(
        impl_params, public_sig.return_type, true, public_sig.is_variadic, public_sig.conv);

    closure_type->resolve<types::closure_t>(capture_span, fn_type, *impl_sig);
    resolving_.set_sema_type(fn_id, *closure_type);
    return *closure_type;
}

auto type_resolver::is_mutable_local(const symbol& sym) const -> bool {
    const auto node{sym.get_data().as_opt<symbols::node_t>()};
    const auto decl{node ? resolving_.ast.get_as_opt<ast::decl_stmt>(*node) : stdx::none};
    return decl && decl->has_modifier(ast::decl_modifiers::MUT);
}

auto type_resolver::const_closure_runtime_capture(const ast::decl_stmt& decl) const
    -> stdx::option<std::string_view> {
    if (!decl.has_modifier(ast::decl_modifiers::COMPTIME) ||
        decl.has_modifier(ast::decl_modifiers::MUT) || !decl.value ||
        !decl.value->is<ast::function_expr>()) {
        return stdx::none;
    }
    for (const auto& capture : resolving_.get_captures(*decl.value)) {
        const auto captured{ctx_.registry.lookup(table_stack_, capture.name)};
        if (!captured) { continue; }
        const auto node{captured->get_data().as_opt<symbols::node_t>()};
        const auto captured_decl{node ? resolving_.ast.get_as_opt<ast::decl_stmt>(*node)
                                      : stdx::none};
        if (captured_decl && !captured_decl->has_modifier(ast::decl_modifiers::COMPTIME)) {
            return capture.name;
        }
    }
    return stdx::none;
}

// `auto`/`type`/`comptime`/pack parameters (or `impl` bounds) make a function generic
auto type_resolver::declares_generic_params(const ast::function_expr& fn_expr) const -> bool {
    if (!fn_expr.impl_bounds.empty()) { return true; }
    return std::ranges::any_of(fn_expr.parameters, [&](const ast::function_expr::parameter& p) {
        if (p.is_comptime || p.is_pack || !p.explicit_type) { return true; }
        return p.spells_type() || spells_auto_pattern(resolving_.ast, p.explicit_type);
    });
}

} // namespace ghoti::sema
