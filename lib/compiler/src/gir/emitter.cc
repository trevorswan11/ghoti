#include "compiler/gir/emitter.hh"

#include <algorithm>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include <gsl/pointers>
#include <gsl/span>
#include <gsl/util>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/gir/builder.hh"
#include "compiler/gir/const_eval.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/layout.hh"
#include "compiler/gir/module.hh"
#include "compiler/gir/segment.hh"
#include "compiler/gir/symbol_scoping.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/generic.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/token_type.hh"
#include "support/int128.hh"

namespace ghoti::gir {

namespace {

// Rewrites ghoti's GCC-style `%N` operand placeholders into LLVM's `$N` form
[[nodiscard]] auto rewrite_asm_template(std::string_view src) -> std::string {
    std::string out;
    out.reserve(src.size());
    for (usize i{0}; i < src.size(); ++i) {
        const char c{src[i]};
        if (c == '$') {
            out += "$$";
            continue;
        }
        if (c == '%' && i + 1 < src.size()) {
            const char next{src[i + 1]};
            if (next == '%') {
                out += '%';
                ++i;
                continue;
            }
            if (next >= '0' && next <= '9') {
                out += '$';
                continue;
            }
        }
        out += c;
    }
    return out;
}

} // namespace

auto emitter::emit(bool include_builtin_test_runtime) -> module {
    PROFILE_FUNCTION();
    {
        PROFILE_SCOPE("emitter: resolve deferred types");
        const_eval_.resolve_all_deferred_types();
    }

    const auto emit_top_level_stmt = [&](mod::module& module, ast::node_id id) {
        return module.ast[id].visit(
            [&](const auto&) {},
            [&](const ast::decl_stmt& decl) { emit_top_level_decl(id, decl); },
            [&](const ast::using_stmt& using_stmt) { emit_top_level_using(id, using_stmt); },
            [&](const ast::impl_stmt& impl) { emit_top_level_impl(id, impl); },
            [&](const ast::test_stmt& test) { emit_top_level_test(id, test); });
    };

    // Traverse all transitively imported modules reachable from ast_module_
    std::vector<mod::module*>                  imported_mods;
    ankerl::unordered_dense::set<mod::module*> visited;
    visited.insert(&ast_module_);

    auto collect_imported = [&](auto& self, mod::module& cur) -> void {
        for (const auto import_id : cur.import_nodes) {
            if (const auto sema_type{cur.get_sema_type_opt(import_id)}) {
                if (const auto m_data{sema_type->get_data().as_opt<sema::types::module>()}) {
                    auto& dep{m_data->imported};
                    if (visited.insert(&dep).second) {
                        imported_mods.emplace_back(&dep);
                        self(self, dep);
                    }
                }
            }
        }
    };
    collect_imported(collect_imported, ast_module_);

    // The compiler-provided `builtin` module is injected into the prelude, not imported
    if (include_builtin_test_runtime && ctx_.modules.has_builtin_module()) {
        auto& builtin_mod{ctx_.modules.builtin_module()};
        if (visited.insert(&builtin_mod).second) { imported_mods.emplace_back(&builtin_mod); }
    }

    {
        PROFILE_SCOPE("emitter: resolve deferred types in imported modules");
        for (auto* m : imported_mods) {
            const_eval evaluator{ctx_, *m};
            evaluator.resolve_all_deferred_types();
        }
    }

    // With every participating module known, decide which same-named symbols must be qualified.
    symbol_scoping_ = symbol_scoping::build(ctx_, ast_module_, imported_mods);
    const_eval_.set_symbol_scoping(symbol_scoping_);

    {
        PROFILE_SCOPE("emitter: emit root module");
        for (const auto root_id : ast_module_.ast) { emit_top_level_stmt(ast_module_, root_id); }
        for (const auto& inst : ast_module_.generic_instantiations) {
            emit_generic_instantiation(inst);
        }
        for (const auto& tcm : ast_module_.type_ctor_member_emits) {
            emit_type_ctor_member(ast_module_, tcm);
        }
        for (const auto& tcm : ast_module_.impl_ctor_member_emits) {
            emit_type_ctor_member(ast_module_, tcm);
        }
    }

    gir_module_.mark_import_boundary();

    {
        PROFILE_SCOPE("emitter: emit imported modules");
        for (auto* other_mod : imported_mods) {
            if (!other_mod || other_mod->is_poisoned() || other_mod->is_errored()) { continue; }
            auto prev_module{std::exchange(active_module_, other_mod)};
            const_eval_.set_module(*other_mod);
            for (const auto root_id : other_mod->ast) { emit_top_level_stmt(*other_mod, root_id); }
            for (const auto& inst : other_mod->generic_instantiations) {
                emit_generic_instantiation(inst);
            }
            for (const auto& tcm : other_mod->type_ctor_member_emits) {
                emit_type_ctor_member(*other_mod, tcm);
            }
            for (const auto& tcm : other_mod->impl_ctor_member_emits) {
                emit_type_ctor_member(*other_mod, tcm);
            }
            active_module_ = prev_module;
            const_eval_.set_module(*prev_module);
        }
    }

    for (const auto name : pending_builtin_runtime_) { ensure_builtin_runtime(name); }
    return std::move(gir_module_);
}

auto emitter::emit_generic_instantiation(const sema::generic_instantiation_request& req) -> void {
    PROFILE_FUNCTION();
    if (gir_module_.has_function(req.mangled_name)) { return; }
    if (req.return_type->get_kind() == sema::type_kind::TYPE) { return; }

    mod::module& fn_mod{*req.module};
    const auto   fn_expr_opt{fn_mod.ast[req.fn_node_id].visit(
        [&](const auto&) -> stdx::option<const ast::function_expr&> { return stdx::none; },
        [&](const ast::decl_stmt& decl) -> stdx::option<const ast::function_expr&> {
            if (decl.value) { return fn_mod.ast.get_as_opt<ast::function_expr>(*decl.value); }
            return stdx::none;
        },
        [&](const ast::function_expr& fn_expr) -> stdx::option<const ast::function_expr&> {
            return fn_expr;
        })};

    VERIFY(fn_expr_opt, "Generic instantiation must reference a valid function expression");
    const auto& fn_expr{*fn_expr_opt};

    sema::types::key_t fn_key{sema::type_kind::FUNCTION, sema::types::mut::CONSTANT};
    for (const auto& param_type : req.arg_types) { fn_key.imprint(*param_type); }
    fn_key.imprint(*req.return_type);
    auto& fn_type{*ctx_.pool[fn_key]};
    fn_type.resolve_if<sema::types::function>(req.arg_types, *req.return_type, false, false);

    auto& fn{gir_module_.add_function(req.mangled_name, fn_type, false, false)};
    auto& entry{fn.add_segment()};
    builder_.set_insert_point(fn, entry);

    auto prev_module{std::exchange(active_module_, &fn_mod)};
    const_eval_.set_module(fn_mod);

    // Re-bind `constexpr` parameters so `const_eval` folds them the same way it did at resolution
    sema::constexpr_frame                                                             cx_frame;
    ankerl::unordered_dense::map<std::string_view, gsl::not_null<const const_value*>> cx_by_name;
    if (const auto cx_args{ctx_.instantiation_cache.get_constexpr_args(req.mangled_name)}) {
        usize cx_i{0};
        for (const auto& param : fn_expr.parameters) {
            if (!param.is_constexpr || cx_i >= cx_args->size()) { continue; }
            const auto& p_name{fn_mod.ast.get_as<ast::identifier_expr>(param.name).name};
            cx_frame.insert_or_assign(p_name, (*cx_args)[cx_i]);
            cx_by_name.emplace(p_name, &(*cx_args)[cx_i]);
            ++cx_i;
        }
    }
    const constexpr_frame_guard cx_frame_guard{ctx_.constexpr_binding_frames, std::move(cx_frame)};

    // Replay this monomorphization's body typing onto the shared AST nodes before emitting
    const_eval_.clear_memo();
    if (const auto diff{ctx_.instantiation_cache.get_body_type_diff(req.mangled_name)}) {
        for (const auto& [idx, ty] : diff->node_types) {
            fn_mod.sema_side_tables.node_types.values[idx] = ty;
        }
        for (const auto& [idx, ty] : diff->explicit_types) {
            fn_mod.sema_side_tables.explicit_types.values[idx] = ty;
        }
        for (const auto& [idx, br] : diff->if_branches) {
            fn_mod.if_constexpr_results.insert_or_assign(idx, br);
        }
        for (const auto& [idx, arm] : diff->match_arms) {
            fn_mod.match_arm_results.insert_or_assign(idx, arm);
        }
    }
    {
        const scope_guard g{scopes_};
        usize             rt_i{0};
        for (const auto& param : fn_expr.parameters) {
            const auto& p_name{fn_mod.ast.get_as<ast::identifier_expr>(param.name).name};

            // `constexpr` parameters are erased from the signature
            if (param.is_constexpr) {
                const auto cxit{cx_by_name.find(p_name)};
                if (cxit == cx_by_name.end()) { continue; }
                const auto& val{*cxit->second};
                auto&       btype{val.get_type() ? *val.get_type() : fn_type};
                value       bound{value{void_val{}, btype}};
                if (val.is<const_closure>()) {
                    bound = value{emit_constexpr_closure(val.as<const_closure>()), btype};
                } else if (val.is<std::string>()) {
                    bound = value{val.as<std::string>(), btype};
                } else {
                    bound = materialize_const(val);
                }
                scopes_.back().bindings.emplace(p_name,
                                                local_binding{
                                                    .id        = {0, local_kind::TEMPORARY},
                                                    .type      = btype,
                                                    .is_alloca = false,
                                                    .const_val = std::move(bound),
                                                });
                continue;
            }

            auto& arg_type{req.arg_types[rt_i++]};
            auto& p_slot{fn.add_param(std::string{p_name}, *arg_type)};
            if (arg_type->get_kind() == sema::type_kind::CLOSURE) {
                // A closure argument arrives by value
                const auto spilled{spill_to_temporary(value{p_slot.id, *arg_type}, *arg_type)};
                scopes_.back().bindings.emplace(p_name,
                                                local_binding{
                                                    .id        = spilled.data.as<local_id>(),
                                                    .type      = *arg_type,
                                                    .is_alloca = true,
                                                    .const_val = stdx::none,
                                                });
                continue;
            }
            const bool p_spilled{arg_type->get_kind() == sema::type_kind::SLICE};
            scopes_.back().bindings.emplace(p_name,
                                            local_binding{
                                                .id        = p_slot.id,
                                                .type      = *arg_type,
                                                .is_alloca = p_spilled,
                                                .const_val = stdx::none,
                                            });
        }

        emit_block(fn_mod.ast.get_as<ast::block_stmt>(fn_expr.body));
        if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
            if (req.return_type->get_kind() == sema::type_kind::VOID_) {
                builder_.emit_return();
            } else {
                builder_.emit_return(value{undefined_val{}, *req.return_type});
            }
        }
    }

    active_module_ = prev_module;
    if (prev_module) { const_eval_.set_module(*prev_module); }
}

auto emitter::emit_type_ctor_member(mod::module& owner_mod, const sema::type_ctor_member_emit& tcm)
    -> void {
    PROFILE_FUNCTION();
    if (gir_module_.has_function(tcm.gir_name)) { return; }

    const auto& decl{owner_mod.ast.get_as<ast::decl_stmt>(tcm.member_decl)};
    const auto  fn_expr{owner_mod.ast.get_as_opt<ast::function_expr>(*decl.value)};
    if (!fn_expr) { return; }

    auto prev_module{std::exchange(active_module_, &owner_mod)};
    const_eval_.set_module(owner_mod);
    const_eval_.clear_memo();

    // Replay this constructor instantiation's body typing onto the shared AST nodes so
    // `@this()` / `.{ ... }` / `^self` resolve to the right shape.
    if (const auto diff{ctx_.instantiation_cache.get_body_type_diff(tcm.typing_key)}) {
        for (const auto& [idx, ty] : diff->node_types) {
            owner_mod.sema_side_tables.node_types.values[idx] = ty;
        }
        for (const auto& [idx, ty] : diff->explicit_types) {
            owner_mod.sema_side_tables.explicit_types.values[idx] = ty;
        }
        for (const auto& [idx, br] : diff->if_branches) {
            owner_mod.if_constexpr_results.insert_or_assign(idx, br);
        }
        for (const auto& [idx, arm] : diff->match_arms) {
            owner_mod.match_arm_results.insert_or_assign(idx, arm);
        }
    }

    // Make this constructor instantiation's `constexpr` parameter values visible to the bodyA
    sema::constexpr_frame ctor_frame;
    if (const auto bindings{ctx_.instantiation_cache.get_type_ctor_bindings(tcm.typing_key)}) {
        for (const auto& [name, val] : *bindings) { ctor_frame.insert_or_assign(name, val); }
    }
    const constexpr_frame_guard ctor_binding_guard{ctx_.constexpr_binding_frames,
                                                   std::move(ctor_frame)};

    user_type_stack_.emplace_back(tcm.owner_clone);
    emit_function(tcm.member_decl, decl, *fn_expr, tcm.gir_name);
    user_type_stack_.pop_back();

    active_module_ = prev_module;
    if (prev_module) { const_eval_.set_module(*prev_module); }
}

namespace {

auto get_decl_linkage(const ast::decl_stmt& decl) noexcept -> gir::linkage {
    if (decl.has_modifier(ast::decl_modifiers::EXPORT)) { return gir::linkage::EXPORT; }
    if (decl.has_modifier(ast::decl_modifiers::PUBLIC)) { return gir::linkage::PUBLIC; }
    if (decl.has_modifier(ast::decl_modifiers::EXTERN)) { return gir::linkage::EXTERN; }
    return gir::linkage::INTERNAL;
}

// A `const X := @compileError("msg")` declaration is a deferred error
auto decl_is_deferred_compile_error(const ast::AST& ast, const ast::decl_stmt& decl) -> bool {
    if (!decl.value) { return false; }
    const ast::node_id value_id{*decl.value};
    if (value_id.get_kind() != ast::node_kind::CALL_EXPRESSION) { return false; }
    const auto& call{ast.get_as<ast::call_expr>(value_id)};
    return ast::node_id{call.function}.get_token_type() ==
           syntax::token_type_t::BUILTIN_COMPILE_ERROR;
}

// Resolves an extern decl's optional `("target")` argument, defaulting to `"c"`.
auto get_extern_target(const ast::AST& ast, const ast::decl_stmt& decl) -> std::string {
    if (!decl.extern_target) { return "c"; }
    return std::string{ast.get_as<ast::string_expr>(**decl.extern_target).value};
}

// The explicit symbol name from `extern("lib", "sym")` / `export("sym")`, else empty.
auto get_link_name(const ast::AST& ast, const ast::decl_stmt& decl) -> std::string {
    if (!decl.link_name) { return {}; }
    return std::string{ast.get_as<ast::string_expr>(**decl.link_name).value};
}

} // namespace

auto emitter::emit_slice_from_array(value arr_lval, const sema::type& arr_type) -> value {
    PROFILE_FUNCTION();
    const auto arr_data{arr_type.get_data().as_opt<sema::types::array>()};
    ASSERT(arr_data, "Array-to-slice decay requires an array sema type");

    const auto mutability{arr_type.is_constant() ? sema::types::mut::CONSTANT
                                                 : sema::types::mut::MUTABLE};
    auto&      ptr_type{ctx_.get_pointer(mutability, arr_data->underlying)};
    auto& slice_type{ctx_.get_slice(mutability, arr_data->null_terminated, arr_data->underlying)};
    auto& usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};

    const auto slice_slot{builder_.emit_alloca(slice_type)};
    const auto elem0_ptr{builder_.emit_get_element_ptr(
        arr_lval, {value{static_cast<u64>(0), usize_type}}, ptr_type)};

    const auto field0_ptr{builder_.emit_get_element_ptr(
        value{slice_slot, slice_type}, {value{SLICE_PTR_FIELD_INDEX, usize_type}}, ptr_type)};
    builder_.emit_store(value{field0_ptr, ptr_type}, value{elem0_ptr, ptr_type}).is_initializer =
        true;

    const auto field1_ptr{builder_.emit_get_element_ptr(
        value{slice_slot, slice_type}, {value{SLICE_LEN_FIELD_INDEX, usize_type}}, usize_type)};
    builder_
        .emit_store(value{field1_ptr, usize_type},
                    value{static_cast<u64>(arr_data->len), usize_type})
        .is_initializer = true;

    const auto loaded_slice{builder_.emit_load(value{slice_slot, slice_type}, slice_type)};
    return value{loaded_slice, slice_type};
}

auto emitter::emit_dyn_coercion(ast::expr_handle src, const sema::type& fat_type) -> value {
    PROFILE_FUNCTION();
    const auto  p{fat_type.get_data().as_opt<sema::types::pointer>()};
    const auto  r{fat_type.get_data().as_opt<sema::types::reference>()};
    auto&       dyn_type{p ? p->underlying : r->underlying};
    const auto& dyn{dyn_type.get_data().as<sema::types::dyn_t>()};
    const auto& iface{dyn.interface.get_data().as<sema::types::interface_t>()};

    // The concrete `T` behind the source `&T` / `^T`.
    auto* target{const_cast<sema::type*>(active_mod().get_sema_type_opt(*src).get())};
    if (const auto tp{target->get_data().as_opt<sema::types::pointer>()}) {
        target = &tp->underlying;
    }
    if (const auto tr{target->get_data().as_opt<sema::types::reference>()}) {
        target = &tr->underlying;
    }
    const auto rec{ctx_.impls.lookup(*target, dyn.interface)};

    auto& ptr_ty{ctx_.get_pointer(sema::types::mut::CONSTANT,
                                  ctx_.get_builtin_resolved_type(sema::type_kind::OPAQUE))};
    auto& usize_ty{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};

    // Register one private vtable global per `(I, T)`; slots hold the impl methods in order.
    const auto vtable_sym{fmt::format("__vtable.{}", rec ? rec->body_scope_idx : 0UZ)};
    if (rec && std::ranges::none_of(active_mod().dyn_vtables,
                                    [&](const auto& v) { return v.symbol == vtable_sym; })) {
        mod::module::dyn_vtable v{.symbol = vtable_sym, .slots = {}};
        for (const auto name : iface.method_names) {
            v.slots.emplace_back(symbol_scoping_.name_for(rec->body_scope_idx, name));
        }
        active_mod().dyn_vtables.emplace_back(std::move(v));
    }

    // The `data` half is the address of the source object
    const auto src_ty{active_mod().get_sema_type_opt(*src)};
    const auto data_ptr{src_ty && (src_ty->get_kind() == sema::type_kind::POINTER ||
                                   src_ty->get_kind() == sema::type_kind::REFERENCE)
                            ? value{emit_expression_id_raw(*src).data, ptr_ty}
                            : value{builder_.emit_address_of(emit_lvalue(src), ptr_ty), ptr_ty}};
    const auto vtable_addr{builder_.emit_global_addr(vtable_sym, ptr_ty, true)};

    // Build `{ data, vtable }` in an alloca typed by the bare `dyn` then load it out.
    auto&      dyn_mut{p ? const_cast<sema::type&>(p->underlying)
                         : const_cast<sema::type&>(r->underlying)};
    const auto slot{builder_.emit_alloca(dyn_mut)};
    const auto f0{
        builder_.emit_get_element_ptr(value{slot, dyn_mut}, {value{u64{0}, usize_ty}}, ptr_ty)};
    builder_.emit_store(value{f0, ptr_ty}, value{data_ptr.data, ptr_ty}).is_initializer = true;
    const auto f1{
        builder_.emit_get_element_ptr(value{slot, dyn_mut}, {value{u64{1}, usize_ty}}, ptr_ty)};
    builder_.emit_store(value{f1, ptr_ty}, value{vtable_addr, ptr_ty}).is_initializer = true;

    auto& fat_mut{const_cast<sema::type&>(fat_type)};
    return value{builder_.emit_load(value{slot, dyn_mut}, fat_mut), fat_mut};
}

auto emitter::folded_int(const value& v) noexcept -> stdx::option<i128> {
    if (const auto x{v.as_opt<i64>()}) { return static_cast<i128>(*x); }
    if (const auto x{v.as_opt<i128>()}) { return *x; }
    if (const auto x{v.as_opt<u64>()}) { return static_cast<i128>(*x); }
    if (const auto x{v.as_opt<u128>()}) { return static_cast<i128>(*x); }
    return stdx::none;
}

auto emitter::coerce_constexpr_int(value v, sema::type& target, ast::node_id at) -> value {
    if (const auto folded{folded_int(v)};
        folded && !sema::constexpr_int_fits(*folded, target, target_ptr_bits_)) {
        ctx_.diags.emplace_back(fmt::format("integer literal is out of range for type '{}'",
                                            sema::type_kind_display_name(target)),
                                sema::error::LITERAL_OUT_OF_RANGE,
                                active_ast().location_of(at));
    }
    v.type.emplace(target);
    return v;
}

auto emitter::emit_coerced_expr(ast::expr_handle expr_id, const sema::type& dest_type) -> value {
    PROFILE_FUNCTION();
    // Build the fat pointer for `&T` / `^T` -> `&dyn I` / `^dyn I`
    const auto dest_dyn{[&] -> stdx::option<const sema::type&> {
        if (const auto p{dest_type.get_data().as_opt<sema::types::pointer>()}) {
            return p->underlying.get_kind() == sema::type_kind::DYN
                       ? stdx::option<const sema::type&>{p->underlying}
                       : stdx::none;
        }
        if (const auto r{dest_type.get_data().as_opt<sema::types::reference>()}) {
            return r->underlying.get_kind() == sema::type_kind::DYN
                       ? stdx::option<const sema::type&>{r->underlying}
                       : stdx::none;
        }
        return stdx::none;
    }()};

    if (dest_dyn) {
        const auto                      src_ty{active_mod().get_sema_type_opt(*expr_id)};
        stdx::option<const sema::type&> src_ref;
        if (src_ty) {
            if (const auto p{src_ty->get_data().as_opt<sema::types::pointer>()}) {
                src_ref.emplace(p->underlying);
            } else if (const auto r{src_ty->get_data().as_opt<sema::types::reference>()}) {
                src_ref.emplace(r->underlying);
            }
        }
        if (src_ref && src_ref->get_kind() != sema::type_kind::DYN) {
            return emit_dyn_coercion(expr_id, dest_type);
        }
    }

    if (dest_type.get_kind() == sema::type_kind::SLICE) {
        if (const auto rhs_type{active_mod().get_sema_type_opt(expr_id)}) {
            if (rhs_type->get_kind() == sema::type_kind::ARRAY) {
                return emit_slice_from_array(emit_lvalue(expr_id), *rhs_type);
            }
        }
    }
    // A reference destination is one of the few positions that must see the raw reference value
    if (dest_type.get_kind() == sema::type_kind::REFERENCE) {
        return emit_expression_id_raw(*expr_id);
    }
    // `undefined` carries no type of its own; adopt the destination's so codegen can size it.
    if (active_ast().get_as_opt<ast::undefined_expr>(*expr_id)) {
        return value{undefined_val{}, const_cast<sema::type&>(dest_type)};
    }

    const auto val{emit_expression(expr_id)};

    // A `constexpr_int` / `constexpr_float` value already carries its exact numeric payload;
    // coercing it to a concrete peer is a pure retype
    if (val.type && sema::is_constexpr_numeric(val.type->get_kind()) &&
        sema::is_numeric(dest_type.get_kind())) {
        auto& concrete{const_cast<sema::type&>(dest_type)};
        if (sema::is_float(dest_type.get_kind()) &&
            val.type->get_kind() == sema::type_kind::CONSTEXPR_INT) {
            // int literal -> float context: convert the payload to floating point.
            if (const auto iv{val.as_opt<i64>()}) { return value{static_cast<f64>(*iv), concrete}; }
            if (const auto uv{val.as_opt<u64>()}) { return value{static_cast<f64>(*uv), concrete}; }
            if (const auto iv{val.as_opt<i128>()}) {
                return value{static_cast<f64>(*iv), concrete};
            }
            if (const auto uv{val.as_opt<u128>()}) {
                return value{static_cast<f64>(*uv), concrete};
            }
        }
        // Reject a compile-time integer that does not fit its concrete integer target.
        if (val.type->get_kind() == sema::type_kind::CONSTEXPR_INT &&
            sema::is_integer(dest_type.get_kind())) {
            return coerce_constexpr_int(val, concrete, *expr_id);
        }
        return value{val.data, concrete};
    }

    // A numeric value narrower than the destination widens implicitly (`iW`->`iV`, `f32`->`f64`,
    // ...)
    if (val.type && sema::is_numeric(dest_type.get_kind()) &&
        sema::is_numeric(val.type->get_kind()) &&
        !sema::is_same_unqualified(*val.type, dest_type) &&
        sema::is_implicit_widenable(*val.type, dest_type)) {
        auto& widened{const_cast<sema::type&>(dest_type)};
        return value{builder_.emit_cast(instruction_kind::WIDEN_CAST, val, widened), widened};
    }

    return val;
}

auto emitter::emit_top_level_decl(ast::node_id id, const ast::decl_stmt& decl) -> void {
    PROFILE_FUNCTION();
    const auto& name_ident{active_ast().get_as<ast::identifier_expr>(decl.name)};
    const auto  name{name_ident.name};
    const auto  sema_type{active_mod().get_sema_type_opt(id)};
    ASSERT(sema_type, "Top-level declaration must have a resolved sema type");
    if (sema_type->get_kind() == sema::type_kind::TYPE) { return; }
    // An `interface` decl is a pure compile-time contract with no runtime storage or body.
    if (sema_type->get_kind() == sema::type_kind::INTERFACE) { return; }
    // A deferred `@compileError` declaration only reports at its reference sites
    if (decl_is_deferred_compile_error(active_ast(), decl)) { return; }

    const auto linkage{get_decl_linkage(decl)};

    if (decl.value) {
        if (const auto fn_expr{active_ast().get_as_opt<ast::function_expr>(*decl.value)}) {
            if (const auto fn_data{sema_type->get_data().as_opt<sema::types::function>()}) {
                // Generic templates will be emitted via monomorphized instantiations
                if (ctx_.generic_functions.get_opt(*sema_type)) { return; }
                // A `fn(...): type` is a compile-time type constructor with no runtime body.
                if (fn_data->return_type.get_kind() == sema::type_kind::TYPE) { return; }
            }
            return emit_function(id, decl, *fn_expr);
        } else {
            const auto struct_expr{active_ast().get_as_opt<ast::struct_expr>(*decl.value)};
            const auto union_expr{active_ast().get_as_opt<ast::union_expr>(*decl.value)};
            const auto enum_expr{active_ast().get_as_opt<ast::enum_expr>(*decl.value)};
            if (struct_expr || union_expr || enum_expr) {
                // Push the structural type so member fn bodies resolve bare sibling members.
                const auto owner{active_mod().get_sema_type_opt(*decl.value)};
                const bool pushed{owner.has_value()};
                if (pushed) { user_type_stack_.emplace_back(owner.get()); }

                const auto emit_members{[&](auto members) {
                    for (const auto& member : members) {
                        if (const auto md{active_ast().get_as_opt<ast::decl_stmt>(*member)}) {
                            emit_top_level_decl(*member, *md);
                        }
                    }
                }};
                if (struct_expr) {
                    emit_members(struct_expr->members);
                } else if (union_expr) {
                    emit_members(union_expr->members);
                } else {
                    emit_members(enum_expr->members);
                }

                if (pushed) { user_type_stack_.pop_back(); }
            }
        }
    } else if (decl.has_modifier(ast::decl_modifiers::EXTERN)) {
        if (gir_module_.has_function(name)) { return; }
        if (const auto fn_data{sema_type->get_data().as_opt<sema::types::function>()}) {
            auto& fn{gir_module_.add_function(std::string{name},
                                              *sema_type,
                                              false,
                                              false,
                                              fn_data->is_variadic,
                                              gir::linkage::EXTERN,
                                              get_extern_target(active_ast(), decl))};
            fn.set_link_name(get_link_name(active_ast(), decl));
            fn.set_weak(decl.has_modifier(ast::decl_modifiers::WEAK));
            for (usize i{0}; const auto& param : fn_data->params) {
                fn.add_param(fmt::format("param.{}", i++), *param);
            }
            return;
        }
    }

    const auto global_gir_name{def_symbol_name(name)};
    if (gir_module_.has_global(global_gir_name)) { return; }

    const auto is_const{decl.has_modifier(ast::decl_modifiers::CONSTANT) ||
                        decl.has_modifier(ast::decl_modifiers::CONSTEXPR)};

    stdx::option<value>       init_val;
    stdx::option<const_value> const_init;
    if (decl.value && !active_ast().get_as_opt<ast::undefined_expr>(*decl.value)) {
        if (auto cv{const_eval_.try_eval(*decl.value)}) {
            if (is_const && cv->is<std::string>() && cv->get_type() &&
                cv->get_type()->get_kind() == sema::type_kind::FUNCTION) {
                return;
            }
            // A folded struct/array aggregate cannot round-trip through the scalar `value` variant
            if (cv->is<const_struct>() || cv->is<const_array>()) {
                const_init.emplace(std::move(*cv));
            } else {
                init_val.emplace(cv->to_gir_value());
            }
        } else if (!is_const) {
            init_val.emplace(emit_expression(*decl.value));
        }
    }
    auto& g{gir_module_.add_global(global_gir_name,
                                   *sema_type,
                                   is_const,
                                   init_val,
                                   linkage,
                                   get_extern_target(active_ast(), decl))};
    g.const_init      = std::move(const_init);
    g.link_name       = get_link_name(active_ast(), decl);
    g.is_thread_local = decl.has_modifier(ast::decl_modifiers::THREADLOCAL);
    g.is_weak         = decl.has_modifier(ast::decl_modifiers::WEAK);
}

auto emitter::emit_top_level_using(ast::node_id, const ast::using_stmt& using_stmt) -> void {
    PROFILE_FUNCTION();
    const auto& name_ident{active_ast().get_as<ast::identifier_expr>(using_stmt.alias)};
    const auto  sema_type{active_mod().get_sema_type_opt(using_stmt.explicit_type)};
    ASSERT(sema_type, "Using statement explicit type must be resolved");
    gir_module_.add_type(std::string{name_ident.name}, *sema_type);
}

auto emitter::emit_top_level_impl(ast::node_id id, const ast::impl_stmt& impl) -> void {
    PROFILE_FUNCTION();
    if (!impl.impl_params.empty()) { return; } // parameterized: Phase 2.x

    stdx::option<const sema::impl_record&> rec;
    for (const auto* r : ctx_.impls.records()) {
        if (r->enclosing && &*r->enclosing == &active_mod() &&
            r->site.get_index() == id.get_index() && r->site.get_kind() == id.get_kind()) {
            rec.emplace(r);
            break;
        }
    }
    if (!rec || !rec->target_type) { return; }

    auto*            target{const_cast<sema::type*>(rec->target_type.get())};
    const type_guard target_guard{user_type_stack_, target};

    for (const auto& member : impl.members) {
        const auto md{active_ast().get_as_opt<ast::decl_stmt>(*member)};
        if (!md || !md->value) { continue; }
        const auto& mname{active_ast().get_as<ast::identifier_expr>(md->name).name};
        const auto  gir_name{symbol_scoping_.name_for(rec->body_scope_idx, mname)};

        if (const auto fx{active_ast().get_as_opt<ast::function_expr>(*md->value)}) {
            emitting_impl_body_scope_.emplace(rec->body_scope_idx);
            emit_function(*member, *md, *fx, std::string_view{gir_name});
            emitting_impl_body_scope_.reset();
        } else if (const auto block_ty{active_mod().get_sema_type_opt(id)}) {
            // A static member goes in the impl block's own table so refs agree
            const type_guard scope_guard{user_type_stack_, block_ty.get()};
            emit_top_level_decl(*member, *md);
        }
    }

    // Interface default methods the impl inherits
    for (const auto& m : rec->methods) {
        if (!m.inherited || !m.defining_mod || !m.signature.is_valid()) { continue; }
        const auto fx{m.defining_mod->ast.get_as_opt<ast::function_expr>(m.signature)};
        if (!fx || fx->is_type_expr) { continue; }

        // Emit each body once against the interface's  module with that typing replayed.
        emit_impl_default_method(
            symbol_scoping_.name_for(rec->body_scope_idx, m.name),
            rec->body_scope_idx,
            const_cast<mod::module&>(*m.defining_mod),
            m.signature,
            *fx,
            m.fn_type ? stdx::option<sema::type&>{&const_cast<sema::type&>(*m.fn_type)}
                      : stdx::none,
            m.typing_key);
    }
}

auto emitter::emit_impl_default_method(std::string_view          gir_name,
                                       usize                     impl_scope_idx,
                                       mod::module&              iface_mod,
                                       ast::node_id              sig_id,
                                       const ast::function_expr& fn_expr,
                                       stdx::option<sema::type&> concrete_sig,
                                       std::string_view          typing_key) -> void {
    PROFILE_FUNCTION();
    if (gir_module_.has_function(std::string{gir_name})) { return; }
    if (user_type_stack_.empty()) { return; }
    auto* target{user_type_stack_.back()};

    // The default body's AST + resolved types live in the interface's module. Switch to it and
    // replay this impl's per-target typing
    auto       prev_module{std::exchange(active_module_, iface_mod)};
    const auto restore_module{gsl::finally([&] {
        active_module_ = prev_module;
        if (prev_module) { const_eval_.set_module(*prev_module); }
    })};
    const_eval_.set_module(iface_mod);
    const_eval_.clear_memo();
    if (!typing_key.empty()) {
        if (const auto diff{ctx_.instantiation_cache.get_body_type_diff(typing_key)}) {
            for (const auto& [idx, ty] : diff->node_types) {
                iface_mod.sema_side_tables.node_types.values[idx] = ty;
            }
            for (const auto& [idx, ty] : diff->explicit_types) {
                iface_mod.sema_side_tables.explicit_types.values[idx] = ty;
            }
            for (const auto& [idx, br] : diff->if_branches) {
                iface_mod.if_constexpr_results.insert_or_assign(idx, br);
            }
            for (const auto& [idx, arm] : diff->match_arms) {
                iface_mod.match_arm_results.insert_or_assign(idx, arm);
            }
        }
    }

    auto sema_type{concrete_sig ? concrete_sig : active_mod().get_sema_type_opt(sig_id)};
    if (!sema_type) { return; }

    auto& fn{gir_module_.add_function(
        std::string{gir_name}, *sema_type, false, false, fn_expr.variadic, gir::linkage::INTERNAL)};
    auto& entry{fn.add_segment()};
    builder_.set_insert_point(fn, entry);

    const scope_guard        g{scopes_};
    const open_fn_name_guard fn_name_g{open_fn_names_, std::string{gir_name}};
    const_eval_.set_enclosing_type(stdx::option<sema::type&>{*target});

    if (fn_expr.self) {
        const auto& self_ident{active_ast().get_as<ast::identifier_expr>(fn_expr.self->name)};
        const auto  mod{fn_expr.self->modifier};
        sema::type* self_type{target};
        if (mod.is_mutable_ref()) {
            self_type = &ctx_.get_reference(sema::types::mut::MUTABLE, *target);
        } else if (mod.is_const_ref()) {
            self_type = &ctx_.get_reference(sema::types::mut::CONSTANT, *target);
        } else if (mod.is_mutable_ptr()) {
            self_type = &ctx_.get_pointer(sema::types::mut::MUTABLE, *target);
        } else if (mod.is_const_ptr()) {
            self_type = &ctx_.get_pointer(sema::types::mut::CONSTANT, *target);
        }
        auto& self_slot{fn.add_param(std::string{self_ident.name}, *self_type)};
        scopes_.back().bindings.emplace(self_ident.name,
                                        local_binding{
                                            .id        = self_slot.id,
                                            .type      = *self_type,
                                            .is_alloca = false,
                                            .const_val = stdx::none,
                                        });
    }
    for (const auto& param : fn_expr.parameters) {
        const auto& p_ident{active_ast().get_as<ast::identifier_expr>(param.name)};
        const auto  p_type{active_mod().get_sema_type_opt(param.name)};
        if (!p_type) { continue; }
        auto&      p_slot{fn.add_param(std::string{p_ident.name}, *p_type)};
        const bool p_spilled{p_type->get_kind() == sema::type_kind::SLICE};
        scopes_.back().bindings.emplace(
            p_ident.name,
            local_binding{
                .id = p_slot.id, .type = *p_type, .is_alloca = p_spilled, .const_val = stdx::none});
    }

    emitting_impl_default_scope_.emplace(impl_scope_idx);
    emit_block(active_ast().get_as<ast::block_stmt>(fn_expr.body));
    emitting_impl_default_scope_.reset();

    if (const auto cur_seg{builder_.get_segment()}) {
        if (!cur_seg->has_terminator()) {
            if (const auto fd{sema_type->get_data().as_opt<sema::types::function>()};
                fd && fd->return_type.get_kind() != sema::type_kind::VOID_) {
                builder_.emit_return(value{undefined_val{}, fd->return_type});
            } else {
                builder_.emit_return();
            }
        }
    }
}

auto emitter::emit_top_level_test(ast::node_id id, const ast::test_stmt& test) -> void {
    PROFILE_FUNCTION();
    // Every test block shares one canonical `fn(): bool` signature
    auto& bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
    auto& test_fn_type{*ctx_.pool[{sema::type_kind::FUNCTION, sema::types::mut::CONSTANT}]};
    test_fn_type.resolve_if<sema::types::function>(
        gsl::span<sema::type*>{}, bool_type, false, false);

    const auto test_desc{test.description
                             .transform([&](ast::string_handle h) {
                                 return std::string{active_ast().get_as<ast::string_expr>(h).value};
                             })
                             .or_else([this] -> stdx::option<std::string> {
                                 return fmt::format("anonymous_test{}", anon_test_desc_counter_++);
                             })};

    const auto loc{active_ast().location_of(id)};
    const auto unique_fn_name{fmt::format("__ghoti_test_fn_{}", anon_test_fn_counter_++)};

    auto& fn{gir_module_.add_function(unique_fn_name, test_fn_type, true, false)};
    fn.set_test_desc(*test_desc);
    fn.set_test_location(
        active_mod().path.string(), static_cast<u32>(loc.line), static_cast<u32>(loc.column));

    auto& entry{fn.add_segment()};
    builder_.set_insert_point(fn, entry);

    const scope_guard g{scopes_};
    emit_block(active_ast().get_as<ast::block_stmt>(test.block));
    if (const auto cur_seg{builder_.get_segment()}) {
        if (!cur_seg->has_terminator()) { builder_.emit_return(value{true, bool_type}); }
    }
}

auto emitter::emit_function(ast::node_id                   id,
                            const ast::decl_stmt&          decl,
                            const ast::function_expr&      fn_expr,
                            stdx::option<std::string_view> name_override) -> void {
    PROFILE_FUNCTION();
    const auto& name_ident{active_ast().get_as<ast::identifier_expr>(decl.name)};
    const auto  gir_name{name_override ? std::string{*name_override}
                                       : def_symbol_name(name_ident.name)};
    if (gir_module_.has_function(gir_name)) { return; }
    const auto sema_type{active_mod().get_sema_type_opt(id)};
    ASSERT(sema_type, "Function declaration must have a resolved sema type");

    const auto is_constexpr{decl.has_modifier(ast::decl_modifiers::CONSTEXPR)};
    // A name-override emit is a per-instantiation monomorph
    const auto linkage{name_override ? gir::linkage::INTERNAL : get_decl_linkage(decl)};
    auto&      fn{gir_module_.add_function(
        gir_name, *sema_type, false, is_constexpr, fn_expr.variadic, linkage)};
    if (!name_override) { fn.set_link_name(get_link_name(active_ast(), decl)); }
    fn.set_weak(decl.has_modifier(ast::decl_modifiers::WEAK));
    fn.set_naked(fn_expr.is_naked);
    fn.set_calling_conv(fn_expr.conv);

    auto& entry{fn.add_segment()};
    builder_.set_insert_point(fn, entry);
    const scope_guard           g{scopes_};
    const open_fn_name_guard    fn_name_g{open_fn_names_, gir_name};
    const open_fn_closure_guard fn_closure_g{open_fn_is_closure_, false};

    // A member fn body resolves bare sibling static `const` members through const_eval
    const_eval_.set_enclosing_type(user_type_stack_.empty()
                                       ? stdx::none
                                       : stdx::option<sema::type&>{*user_type_stack_.back()});

    if (fn_expr.self) {
        const auto& self_ident{active_ast().get_as<ast::identifier_expr>(fn_expr.self->name)};
        const auto  self_name{self_ident.name};
        const auto  self_type{active_mod().get_sema_type_opt(fn_expr.self->name)};
        ASSERT(self_type, "Self parameter must have a resolved sema type");

        auto&      self_slot{fn.add_param(std::string{self_name}, *self_type)};
        const bool self_spilled{self_type->get_kind() == sema::type_kind::SLICE};
        scopes_.back().bindings.emplace(self_name,
                                        local_binding{
                                            .id        = self_slot.id,
                                            .type      = *self_type,
                                            .is_alloca = self_spilled,
                                            .const_val = stdx::none,
                                        });
    }

    for (const auto& param : fn_expr.parameters) {
        const auto& p_ident{active_ast().get_as<ast::identifier_expr>(param.name)};
        const auto  p_name{p_ident.name};
        const auto  p_type{active_mod().get_sema_type_opt(param.name)};
        ASSERT(p_type, "Function parameter must have a resolved sema type");

        auto&      p_slot{fn.add_param(std::string{p_name}, *p_type)};
        const bool p_spilled{p_type->get_kind() == sema::type_kind::SLICE};
        scopes_.back().bindings.emplace(p_name,
                                        local_binding{
                                            .id        = p_slot.id,
                                            .type      = *p_type,
                                            .is_alloca = p_spilled,
                                            .const_val = stdx::none,
                                        });
    }

    emit_block(active_ast().get_as<ast::block_stmt>(fn_expr.body));
    if (const auto cur_seg{builder_.get_segment()}) {
        if (!cur_seg->has_terminator()) {
            if (fn_expr.is_naked) {
                // A naked body owns its own control flow; never synthesise a return.
                builder_.emit_unreachable();
            } else {
                stdx::option<const sema::types::function&> fn_data;
                auto                                       target_t{sema_type};
                if (target_t) {
                    if (const auto ref{target_t->get_data().as_opt<sema::types::reference>()}) {
                        target_t.emplace(ref->underlying);
                    }
                    if (const auto ptr{target_t->get_data().as_opt<sema::types::pointer>()}) {
                        target_t.emplace(ptr->underlying);
                    }
                    fn_data = target_t->get_data().as_opt<sema::types::function>();
                }
                if (fn_data && fn_data->return_type.get_kind() == sema::type_kind::VOID_) {
                    builder_.emit_return();
                } else if (fn_data) {
                    builder_.emit_return(value{undefined_val{}, fn_data->return_type});
                } else {
                    builder_.emit_return();
                }
            }
        }
    }
}

auto emitter::emit_anonymous_function(ast::node_id id, const ast::function_expr& fn_expr)
    -> std::string {
    PROFILE_FUNCTION();
    const auto sema_type{active_mod().get_sema_type_opt(id)};
    ASSERT(sema_type, "Anonymous function must have a resolved sema type");
    auto&      fn_type{*sema_type};
    const auto anon_name{fmt::format("anonymous_fn{}", anon_fn_counter_++)};

    auto& fn{gir_module_.add_function(anon_name, fn_type, false, false, fn_expr.variadic)};
    fn.set_calling_conv(fn_expr.conv);
    const auto prev_fn{builder_.get_function()};
    const auto prev_seg{builder_.get_segment()};

    auto& entry{fn.add_segment()};
    builder_.set_insert_point(fn, entry);

    {
        const scope_guard g{scopes_};
        for (const auto& param : fn_expr.parameters) {
            const auto& p_ident{active_ast().get_as<ast::identifier_expr>(param.name)};
            const auto  p_name{p_ident.name};
            const auto  p_type{active_mod().get_sema_type_opt(param.name)};
            ASSERT(p_type, "Anonymous function parameter must have a resolved sema type");

            auto&      p_slot{fn.add_param(std::string{p_name}, *p_type)};
            const bool p_spilled{p_type->get_kind() == sema::type_kind::SLICE};
            scopes_.back().bindings.emplace(p_name,
                                            local_binding{
                                                .id        = p_slot.id,
                                                .type      = *p_type,
                                                .is_alloca = p_spilled,
                                                .const_val = stdx::none,
                                            });
        }

        emit_block(active_ast().get_as<ast::block_stmt>(fn_expr.body));
        if (const auto cur_seg{builder_.get_segment()}) {
            if (!cur_seg->has_terminator()) {
                const auto fn_data{fn_type.get_data().as_opt<sema::types::function>()};
                ASSERT(fn_data, "Function type must contain function type data");
                if (fn_data->return_type.get_kind() == sema::type_kind::VOID_) {
                    builder_.emit_return();
                } else {
                    builder_.emit_return(value{undefined_val{}, fn_data->return_type});
                }
            }
        }
    }

    if (prev_fn && prev_seg) { builder_.set_insert_point(*prev_fn, *prev_seg); }
    return anon_name;
}

auto emitter::emit_named_local_function(std::string_view          name,
                                        ast::node_id              id,
                                        const ast::function_expr& fn_expr) -> std::string {
    PROFILE_FUNCTION();
    const auto sema_type{active_mod().get_sema_type_opt(id)};
    ASSERT(sema_type, "Local function must have a resolved sema type");
    auto& fn_type{*sema_type};
    // Deterministic per fn node, so a `constexpr` function-ref argument can name it.
    const auto anon_name{fmt::format("localfn.{}", id.get_index())};
    if (gir_module_.has_function(anon_name)) { return anon_name; }

    auto& fn{gir_module_.add_function(anon_name, fn_type, false, false, fn_expr.variadic)};
    fn.set_calling_conv(fn_expr.conv);
    const auto prev_fn{builder_.get_function()};
    const auto prev_seg{builder_.get_segment()};

    auto& entry{fn.add_segment()};
    builder_.set_insert_point(fn, entry);

    {
        const scope_guard           g{scopes_};
        const open_fn_name_guard    fn_name_g{open_fn_names_, anon_name};
        const open_fn_closure_guard fn_closure_g{open_fn_is_closure_, false};

        // Let a self-referential call by name inside the body resolve to this function
        scopes_.back().bindings.emplace(name,
                                        local_binding{
                                            .id        = local_id{0, local_kind::TEMPORARY},
                                            .type      = fn_type,
                                            .is_alloca = false,
                                            .const_val = value{anon_name, fn_type},
                                            .is_const  = true,
                                        });

        for (const auto& param : fn_expr.parameters) {
            const auto& p_ident{active_ast().get_as<ast::identifier_expr>(param.name)};
            const auto  p_name{p_ident.name};
            const auto  p_type{active_mod().get_sema_type_opt(param.name)};
            ASSERT(p_type, "Local function parameter must have a resolved sema type");

            auto&      p_slot{fn.add_param(std::string{p_name}, *p_type)};
            const bool p_spilled{p_type->get_kind() == sema::type_kind::SLICE};
            scopes_.back().bindings.emplace(p_name,
                                            local_binding{
                                                .id        = p_slot.id,
                                                .type      = *p_type,
                                                .is_alloca = p_spilled,
                                                .const_val = stdx::none,
                                            });
        }

        emit_block(active_ast().get_as<ast::block_stmt>(fn_expr.body));
        if (const auto cur_seg{builder_.get_segment()}) {
            if (!cur_seg->has_terminator()) {
                const auto fn_data{fn_type.get_data().as_opt<sema::types::function>()};
                ASSERT(fn_data, "Function type must contain function type data");
                if (fn_data->return_type.get_kind() == sema::type_kind::VOID_) {
                    builder_.emit_return();
                } else {
                    builder_.emit_return(value{undefined_val{}, fn_data->return_type});
                }
            }
        }
    }

    if (prev_fn && prev_seg) { builder_.set_insert_point(*prev_fn, *prev_seg); }
    return anon_name;
}

auto emitter::emit_closure(ast::node_id id, const ast::function_expr& fn_expr) -> value {
    PROFILE_FUNCTION();
    auto&      closure_type{active_mod().get_sema_type(id)};
    const auto cl{closure_type.get_data().as_opt<sema::types::closure_t>()};
    ASSERT(cl, "Closure expression must have closure type data");

    emit_closure_function(fn_expr, *cl, closure_type);
    return emit_closure_env(*cl, closure_type);
}

auto emitter::emit_closure_function(const ast::function_expr&     fn_expr,
                                    const sema::types::closure_t& cl,
                                    sema::type&                   closure_type) -> void {
    PROFILE_FUNCTION();
    const auto idx{closure_type.get_symbol_table_idx()};
    const auto fn_name{fmt::format("closure{}", idx)};
    if (gir_module_.has_function(fn_name)) { return; }

    const auto impl_sig_data{cl.impl_signature.get_data().as_opt<sema::types::function>()};
    ASSERT(impl_sig_data, "Closure implementation signature must contain function type data");

    auto& fn{gir_module_.add_function(fn_name, cl.impl_signature, false, false, fn_expr.variadic)};
    const auto prev_fn{builder_.get_function()};
    const auto prev_seg{builder_.get_segment()};

    auto& entry{fn.add_segment()};
    builder_.set_insert_point(fn, entry);

    {
        const scope_guard           g{scopes_};
        const open_fn_name_guard    fn_name_g{open_fn_names_, fn_name};
        const open_fn_closure_guard fn_closure_g{open_fn_is_closure_, true};

        auto& self_type{*impl_sig_data->params[0]};
        auto& self_slot{fn.add_param("self", self_type)};
        scopes_.back().bindings.emplace("self",
                                        local_binding{
                                            .id        = self_slot.id,
                                            .type      = self_type,
                                            .is_alloca = false,
                                            .const_val = stdx::none,
                                        });

        for (const auto& param : fn_expr.parameters) {
            const auto& p_ident{active_ast().get_as<ast::identifier_expr>(param.name)};
            const auto  p_name{p_ident.name};
            const auto  p_type{active_mod().get_sema_type_opt(param.name)};
            ASSERT(p_type, "Closure parameter must have a resolved sema type");
            auto&      p_slot{fn.add_param(std::string{p_name}, *p_type)};
            const bool p_spilled{p_type->get_kind() == sema::type_kind::SLICE};
            scopes_.back().bindings.emplace(p_name,
                                            local_binding{
                                                .id        = p_slot.id,
                                                .type      = *p_type,
                                                .is_alloca = p_spilled,
                                                .const_val = stdx::none,
                                            });
        }

        // Load every capture once, up front, from `self`
        auto& usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
        for (usize field_idx{0}; const auto& capture : cl.captures) {
            const auto field_ptr{
                builder_.emit_get_element_ptr(value{self_slot.id, self_type},
                                              {value{static_cast<u64>(field_idx), usize_type}},
                                              *capture.storage_type)};
            const auto loaded{
                builder_.emit_load(value{field_ptr, *capture.storage_type}, *capture.storage_type)};
            const bool by_ref{capture.mode != sema::types::capture_mode::VALUE};
            auto&      local_type{by_ref ? *capture.captured_type : *capture.storage_type};
            scopes_.back().bindings.emplace(capture.name,
                                            local_binding{
                                                .id        = loaded,
                                                .type      = local_type,
                                                .is_alloca = by_ref,
                                                .const_val = stdx::none,
                                            });
            ++field_idx;
        }

        emit_block(active_ast().get_as<ast::block_stmt>(fn_expr.body));
        if (const auto cur_seg{builder_.get_segment()}) {
            if (!cur_seg->has_terminator()) {
                if (impl_sig_data->return_type.get_kind() == sema::type_kind::VOID_) {
                    builder_.emit_return();
                } else {
                    builder_.emit_return(value{undefined_val{}, impl_sig_data->return_type});
                }
            }
        }
    }

    if (prev_fn && prev_seg) { builder_.set_insert_point(*prev_fn, *prev_seg); }
}

auto emitter::emit_closure_env(const sema::types::closure_t& cl, sema::type& closure_type)
    -> value {
    PROFILE_FUNCTION();
    const auto slot{builder_.emit_alloca(closure_type)};
    auto&      usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
    for (usize field_idx{0}; const auto& capture : cl.captures) {
        const auto field_val{get_capture_source(capture)};
        const auto field_ptr{
            builder_.emit_get_element_ptr(value{slot, closure_type},
                                          {value{static_cast<u64>(field_idx), usize_type}},
                                          *capture.storage_type)};
        builder_.emit_store(value{field_ptr, *capture.storage_type}, field_val).is_initializer =
            true;
        ++field_idx;
    }
    const auto loaded{builder_.emit_load(value{slot, closure_type}, closure_type)};
    return value{loaded, closure_type};
}

auto emitter::get_capture_source(const sema::types::closure_capture& capture) -> value {
    const auto binding{lookup_binding(capture.name)};
    ASSERT(binding, "Captured variable must have a binding at its definition site");

    if (capture.mode == sema::types::capture_mode::VALUE) {
        if (binding->const_val) { return *binding->const_val; }
        if (binding->is_alloca) {
            return value{builder_.emit_load(binding->id, binding->type), binding->type};
        }
        return value{binding->id, binding->type};
    }

    // Otherwise a reference, so the address is needed, not the value
    ASSERT(binding->is_alloca, "Reference-captured variable must be addressable");
    return value{binding->id, *capture.storage_type};
}

auto emitter::emit_constexpr_closure(const const_closure& cl) -> std::string {
    PROFILE_FUNCTION();
    const_value       key{const_value::data_t{cl}};
    const std::string fn_name{fmt::format("closureconst.{}", key.mangle())};
    if (gir_module_.has_function(fn_name)) { return fn_name; }

    auto&       def_mod{cl.module ? *cl.module : ast_module_};
    const auto& fn_expr{def_mod.ast.get_as<ast::function_expr>(cl.fn_node)};
    auto&       node_type{def_mod.get_sema_type(cl.fn_node)};
    // A capture-less closure value may wrap a plain function; use its public call signature.
    auto&      sig{node_type.get_data().as_opt<sema::types::closure_t>()
                       ? node_type.get_data().as<sema::types::closure_t>().signature
                       : node_type};
    const auto sig_data{sig.get_data().as_opt<sema::types::function>()};
    ASSERT(sig_data, "constexpr callable must have a function signature");

    auto&      fn{gir_module_.add_function(fn_name, sig, false, false, fn_expr.variadic)};
    const auto prev_fn{builder_.get_function()};
    const auto prev_seg{builder_.get_segment()};
    auto       prev_module{std::exchange(active_module_, &def_mod)};
    const_eval_.set_module(def_mod);

    auto& entry{fn.add_segment()};
    builder_.set_insert_point(fn, entry);
    {
        const scope_guard g{scopes_};

        // Captures fold both in const-eval and as runtime constants inside the body.
        sema::constexpr_frame cx_frame;
        for (const auto& [name, val] : cl.captures.fields) {
            cx_frame.insert_or_assign(std::string_view{name}, val);
            scopes_.back().bindings.emplace(
                std::string_view{name},
                local_binding{
                    .id        = {0, local_kind::TEMPORARY},
                    .type      = val.get_type().value_or(*sig_data->params.front()),
                    .is_alloca = false,
                    .const_val = materialize_const(val),
                });
        }
        const constexpr_frame_guard cxg{ctx_.constexpr_binding_frames, std::move(cx_frame)};

        for (const auto& param : fn_expr.parameters) {
            const auto& p_ident{def_mod.ast.get_as<ast::identifier_expr>(param.name)};
            const auto  p_name{p_ident.name};
            const auto  p_type{def_mod.get_sema_type_opt(param.name)};
            ASSERT(p_type, "closure parameter must have a resolved sema type");
            auto&      p_slot{fn.add_param(std::string{p_name}, *p_type)};
            const bool p_spilled{p_type->get_kind() == sema::type_kind::SLICE};
            scopes_.back().bindings.emplace(p_name,
                                            local_binding{
                                                .id        = p_slot.id,
                                                .type      = *p_type,
                                                .is_alloca = p_spilled,
                                                .const_val = stdx::none,
                                            });
        }

        emit_block(def_mod.ast.get_as<ast::block_stmt>(fn_expr.body));
        if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
            if (sig_data->return_type.get_kind() == sema::type_kind::VOID_) {
                builder_.emit_return();
            } else {
                builder_.emit_return(value{undefined_val{}, sig_data->return_type});
            }
        }
    }

    active_module_ = prev_module;
    if (prev_module) { const_eval_.set_module(*prev_module); }
    if (prev_fn && prev_seg) { builder_.set_insert_point(*prev_fn, *prev_seg); }
    return fn_name;
}

auto emitter::emit_stmt(const ast::stmt_handle& stmt) -> void {
    PROFILE_FUNCTION();
    const auto stmt_id{*stmt};
    builder_.set_location(active_ast().location_of(stmt_id));
    active_ast()[stmt_id].visit(
        [&](const auto&) { UNREACHABLE("Unhandled statement node variant in emit_stmt"); },
        [&](const ast::block_stmt& block) { emit_block(block); },
        [&](const ast::decl_stmt& decl) { emit_decl_stmt(stmt_id, decl); },
        [&](const ast::return_stmt& ret) { emit_return_stmt(stmt_id, ret); },
        [&](const ast::defer_stmt& def) { emit_defer_stmt(stmt_id, def); },
        [&](const ast::expr_stmt& expr_st) { emit_expression_id(expr_st.expression); },
        [&](const ast::break_stmt& brk) { emit_break(stmt_id, brk); },
        [&](const ast::continue_stmt& cnt) { emit_continue(stmt_id, cnt); },
        [&](const ast::discard_stmt& discard) { emit_expression(discard.discarded); },
        // A local `import` only brings a name into scope; the module is emitted separately.
        [&](const ast::import_stmt&) {});
}

auto emitter::retype_if_undefined(value v, sema::type& result_type) -> value {
    if (v.is<undefined_val>()) { return value{undefined_val{}, result_type}; }
    return v;
}

auto emitter::emit_stmt_as_value(const ast::stmt_handle& stmt) -> value {
    PROFILE_FUNCTION();
    return active_ast()[stmt].visit(
        [&](const auto&) -> value {
            emit_stmt(stmt);
            return value{void_val{}, ctx_.get_builtin_resolved_type(sema::type_kind::VOID_)};
        },
        [&](const ast::expr_stmt& expr_st) -> value {
            return emit_expression_id(expr_st.expression);
        },
        [&](const ast::block_stmt& block) -> value {
            emit_block(block);
            return value{void_val{}, ctx_.get_builtin_resolved_type(sema::type_kind::VOID_)};
        });
}

auto emitter::emit_defers_for_scope(usize scope_idx) -> void {
    PROFILE_FUNCTION();
    if (scope_idx >= scopes_.size()) { return; }
    const auto defers{scopes_[scope_idx].defers};
    for (const auto& def_stmt : defers | std::views::reverse) { emit_stmt(def_stmt); }
}

auto emitter::emit_defers_up_to(usize target_depth) -> void {
    PROFILE_FUNCTION();
    if (scopes_.empty()) { return; }
    for (usize i{scopes_.size()}; i > target_depth; --i) { emit_defers_for_scope(i - 1); }
}

auto emitter::emit_defer_stmt(ast::node_id, const ast::defer_stmt& def) -> void {
    PROFILE_FUNCTION();
    ASSERT(!scopes_.empty(), "Defer statement must be within an active scope");
    scopes_.back().defers.emplace_back(def.deferred);
}

auto emitter::emit_break(ast::node_id, const ast::break_stmt& brk) -> void {
    PROFILE_FUNCTION();
    ASSERT(!loop_stack_.empty(), "Break statement must be within an active loop");

    stdx::option<std::string_view> target_label;
    if (brk.label) {
        const auto& ident{active_ast().get_as<ast::identifier_expr>(*brk.label)};
        target_label.emplace(ident.name);
    }

    for (usize idx{loop_stack_.size()}; idx > 0; --idx) {
        const auto& [label, break_target, continue_target, result_slot] = loop_stack_[idx - 1];
        if (!target_label || label == *target_label) {
            if (brk.expression && result_slot) {
                builder_.emit_store(*result_slot, emit_expression(*brk.expression));
            }
            emit_defers_up_to(idx);
            builder_.emit_goto(break_target);
            return;
        }
    }
}

auto emitter::emit_continue(ast::node_id, const ast::continue_stmt& cnt) -> void {
    PROFILE_FUNCTION();
    ASSERT(!loop_stack_.empty(), "Continue statement must be within an active loop");

    stdx::option<std::string_view> target_label;
    if (cnt.label) {
        const auto& ident{active_ast().get_as<ast::identifier_expr>(*cnt.label)};
        target_label.emplace(ident.name);
    }

    for (usize idx{loop_stack_.size()}; idx > 0; --idx) {
        const auto& [label, break_target, continue_target, result_slot]{loop_stack_[idx - 1]};
        if (!target_label || label == *target_label) {
            emit_defers_up_to(idx);
            builder_.emit_goto(continue_target);
            return;
        }
    }
}

auto emitter::emit_block(const ast::block_stmt& block) -> void {
    PROFILE_FUNCTION();
    const scope_guard g{scopes_};
    for (const auto& stmt : block.statements) {
        // A folded `if constexpr` arm (or any diverging statement) can terminate the block
        if (const auto seg{builder_.get_segment()}; seg && seg->has_terminator()) { break; }
        emit_stmt(stmt);
    }
    if (const auto seg{builder_.get_segment()}; !seg || !seg->has_terminator()) {
        emit_defers_for_scope(scopes_.size() - 1);
    }
}

auto emitter::emit_decl_stmt(ast::node_id id, const ast::decl_stmt& decl) -> void {
    PROFILE_FUNCTION();
    const auto& name_ident{active_ast().get_as<ast::identifier_expr>(decl.name)};
    const auto  name{name_ident.name};
    const auto  sema_type{active_mod().get_sema_type_opt(id)};
    ASSERT(sema_type, "Local declaration must have a resolved sema type");
    if (sema_type->get_kind() == sema::type_kind::TYPE) { return; }
    if (decl_is_deferred_compile_error(active_ast(), decl)) { return; }

    if (decl.value &&
        decl.value->any<ast::struct_expr, ast::union_expr, ast::enum_expr, ast::interface_expr>()) {
        return;
    }

    const auto is_const{decl.has_modifier(ast::decl_modifiers::CONSTANT) ||
                        decl.has_modifier(ast::decl_modifiers::CONSTEXPR)};

    // Aggregates need one stable address across every use; only non-aggregates can skip storage.
    const auto is_structural{sema::is_structural(sema_type->get_kind())};
    if (is_const && decl.value && !is_structural) {
        // A local plain function is emitted with a synthetic name; pre-bind `name` to it so a
        // self-referential call (by name or @fnCtx()) inside its own body resolves correctly
        if (const auto fn_expr{active_ast().get_as_opt<ast::function_expr>(*decl.value)}) {
            const auto anon_name{emit_named_local_function(name, **decl.value, *fn_expr)};
            scopes_.back().bindings.emplace(name,
                                            local_binding{
                                                .id        = {0, local_kind::TEMPORARY},
                                                .type      = *sema_type,
                                                .is_alloca = false,
                                                .const_val = value{anon_name, *sema_type},
                                                .is_const  = true,
                                            });
            return;
        }

        if (const auto cv{const_eval_.try_eval(*decl.value)}) {
            auto bound{cv->to_gir_value()};
            // `const x: iN = <constexpr literal>` : range-check and pin the concrete type.
            if (decl.explicit_type && bound.type &&
                bound.type->get_kind() == sema::type_kind::CONSTEXPR_INT &&
                sema::is_integer(sema_type->get_kind())) {
                bound = coerce_constexpr_int(bound, *sema_type, *decl.value);
            }
            scopes_.back().bindings.emplace(name,
                                            local_binding{
                                                .id        = {0, local_kind::TEMPORARY},
                                                .type      = *sema_type,
                                                .is_alloca = false,
                                                .const_val = bound,
                                                .is_const  = true,
                                            });
            return;
        }

        const value val{emit_coerced_expr(*decl.value, *sema_type)};

        // A non-foldable `const` binds directly to its initializer's value, bypassing the
        // store-typecheck a `var` alloca would get; re-check the annotated type here.
        if (decl.explicit_type && val.type && !sema::is_assignable(*val.type, *sema_type)) {
            ctx_.diags.emplace_back(
                fmt::format("Type mismatch in store: cannot assign '{}' to '{}'",
                            sema::type_kind_display_name(*val.type),
                            sema::type_kind_display_name(*sema_type)),
                sema::error::TYPE_MISMATCH,
                active_ast().location_of(*decl.value));
        }

        if (const auto lid{val.as_opt<local_id>()}) {
            scopes_.back().bindings.emplace(name,
                                            local_binding{
                                                .id        = *lid,
                                                .type      = *sema_type,
                                                .is_alloca = false,
                                                .const_val = stdx::none,
                                                .is_const  = true,
                                            });
            return;
        }
        scopes_.back().bindings.emplace(name,
                                        local_binding{
                                            .id        = {0, local_kind::TEMPORARY},
                                            .type      = *sema_type,
                                            .is_alloca = false,
                                            .const_val = val,
                                            .is_const  = true,
                                        });
        return;
    }

    const auto slot{builder_.emit_alloca(*sema_type, name, is_const)};
    // A fresh alloca is already uninitialized, so `= undefined` needs no store.
    if (decl.value && !active_ast().get_as_opt<ast::undefined_expr>(*decl.value)) {
        const value val{emit_coerced_expr(*decl.value, *sema_type)};
        builder_.emit_store(slot, val).is_initializer = true;
    }
    scopes_.back().bindings.emplace(name,
                                    local_binding{
                                        .id        = slot,
                                        .type      = *sema_type,
                                        .is_alloca = true,
                                        .const_val = stdx::none,
                                        .is_const  = is_const,
                                    });
}

auto emitter::emit_return_stmt(ast::node_id stmt_id, const ast::return_stmt& ret) -> void {
    PROFILE_FUNCTION();
    stdx::option<value> ret_val;
    if (ret.expression) {
        const auto fn_opt{builder_.get_function()};
        const auto fn_data{fn_opt ? fn_opt->get_type().get_data().as_opt<sema::types::function>()
                                  : nullptr};
        if (fn_data) {
            ret_val.emplace(emit_coerced_expr(*ret.expression, fn_data->return_type));
        } else {
            ret_val.emplace(emit_expression(*ret.expression));
        }
    }
    emit_defers_up_to(0);
    builder_.set_location(active_ast().location_of(stmt_id));
    builder_.emit_return(ret_val);
}

auto emitter::emit_expression_id(ast::node_id id) -> value {
    auto val{emit_expression_id_raw(id)};
    if (val.type && val.type->get_kind() == sema::type_kind::REFERENCE) {
        const auto ref_data{val.type->get_data().as_opt<sema::types::reference>()};
        ASSERT(ref_data, "Reference-kind value must carry reference type data");
        auto&      referent_type{const_cast<sema::type&>(ref_data->underlying)};
        const auto loaded{builder_.emit_load(val, referent_type)};
        return value{loaded, referent_type};
    }
    return val;
}

auto emitter::emit_expression_id_raw(ast::node_id id) -> value {
    PROFILE_FUNCTION();
    ASSERT(id.is_valid(), "Valid node ID expected in emit_expression_id");
    builder_.set_location(active_ast().location_of(id));

    return active_ast()[id].visit(
        [&](const auto&) -> value {
            UNREACHABLE("Unhandled expression node variant in emit_expression_id");
        },
        [&](const ast::int_literal_expr& data) -> value {
            const auto sema_type{active_mod().get_sema_type_opt(id)};
            auto&      t{sema_type ? *sema_type : ctx_.get_int(32, true)};
            // An unsuffixed integer literal in a float context is a float constant.
            if (sema::is_float(t.get_kind()) || t.get_kind() == sema::type_kind::CONSTEXPR_FLOAT) {
                return value{static_cast<f64>(static_cast<i128>(data.value)), t};
            }
            if (sema::is_unsigned_integer(t)) {
                const u128 v{data.value};
                if (v <= static_cast<u128>(std::numeric_limits<u64>::max())) {
                    return value{static_cast<u64>(v), t};
                }
                return value{v, t};
            }
            const auto v{static_cast<i128>(data.value)};
            if (v >= static_cast<i128>(std::numeric_limits<i64>::min()) &&
                v <= static_cast<i128>(std::numeric_limits<i64>::max())) {
                return value{static_cast<i64>(v), t};
            }
            return value{v, t};
        },
        [&](const ast::float_literal_expr& data) -> value {
            const auto sema_type{active_mod().get_sema_type_opt(id)};
            return value{data.value,
                         sema_type ? *sema_type
                                   : ctx_.get_builtin_resolved_type(sema::type_kind::F64)};
        },
        [&](ast::bool_expr) -> value {
            return value{id.get_token_type() == syntax::token_type_t::BOOLEAN_TRUE,
                         ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
        },
        [&](const ast::string_expr& data) -> value {
            return value{std::string{data.value}, active_mod().get_sema_type_opt(id)};
        },
        [&](ast::void_expr) -> value {
            return value{void_val{}, ctx_.get_builtin_resolved_type(sema::type_kind::VOID_)};
        },
        [&](ast::undefined_expr) -> value {
            return value{undefined_val{},
                         ctx_.get_builtin_resolved_type(sema::type_kind::UNDEFINED)};
        },
        [&](ast::nullptr_expr) -> value {
            return value{nullptr_val{}, ctx_.get_builtin_resolved_type(sema::type_kind::NULLPTR)};
        },
        [&](ast::unreachable_expr) -> value {
            // Reaching a `unreachable` is a safety-check violation; `--unsafe` makes it true UB.
            if (runtime_safety_) {
                emit_panic_call("reached unreachable code", id);
            } else {
                builder_.emit_unreachable();
            }
            return value{undefined_val{},
                         ctx_.get_builtin_resolved_type(sema::type_kind::NORETURN)};
        },
        [&](const ast::identifier_expr& data) -> value { return emit_ident(id, data); },
        [&](const ast::function_expr& data) -> value {
            const auto sema_type{active_mod().get_sema_type_opt(id)};
            // A bodyless `fn(...): ret` type expression carries no runtime value.
            if (data.is_type_expr) { return value{void_val{}, sema_type}; }
            if (sema_type && sema_type->get_kind() == sema::type_kind::CLOSURE) {
                return emit_closure(id, data);
            }
            const auto anon_name{emit_anonymous_function(id, data)};
            return value{anon_name, sema_type};
        },
        [&](const ast::if_expr& data) -> value { return emit_if(id, data); },
        [&](const ast::cfg_value_expr&) -> value {
            // The cfg pass settled this value; emit it via the recorded verdict.
            const auto it{active_mod().cfg_value_results.find(id.get_index())};
            if (it != active_mod().cfg_value_results.end()) {
                if (it->second.is_predicate) {
                    return value{it->second.boolean,
                                 ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
                }
                if (it->second.chosen.is_valid()) {
                    return emit_expression_id_raw(it->second.chosen);
                }
            }
            return value{undefined_val{}, active_mod().get_sema_type_opt(id)};
        },
        [&](const ast::match_expr& data) -> value { return emit_match(id, data); },
        [&](const ast::initializer_expr& data) -> value { return emit_initializer(id, data); },
        [&](const ast::dot_expr& data) -> value { return emit_dot(id, data); },
        [&](const ast::index_expr& data) -> value { return emit_index(id, data); },
        [&](const ast::address_of_expr& data) -> value { return emit_address_of(id, data); },
        [&](const ast::dereference_expr& data) -> value { return emit_dereference(id, data); },
        [&](const ast::reference_expr& data) -> value { return emit_reference(id, data); },
        [&](const ast::implicit_access_expr& data) -> value {
            return emit_implicit_access(id, data);
        },
        [&](const ast::module_access_expr& data) -> value { return emit_module_access(id, data); },
        [&](const ast::while_loop_expr& data) -> value { return emit_while(id, data); },
        [&](const ast::do_while_loop_expr& data) -> value { return emit_do_while(id, data); },
        [&](const ast::infinite_loop_expr& data) -> value { return emit_infinite_loop(id, data); },
        [&](const ast::for_loop_expr& data) -> value { return emit_for(id, data); },
        [&](const ast::label_expr& data) -> value { return emit_label(id, data); },
        [&](const ast::binary_expr& data) -> value { return emit_binary(id, data); },
        [&](const ast::unary_expr& data) -> value { return emit_unary(id, data); },
        [&](const ast::unwrap_expr& data) -> value { return emit_unwrap(id, data); },
        [&](const ast::assignment_expr& data) -> value { return emit_assignment(id, data); },
        [&](const ast::call_expr& data) -> value { return emit_call(id, data); },
        [&](const ast::asm_expr& data) -> value { return emit_asm(id, data); },
        [&](const ast::array_expr& data) -> value { return emit_array(id, data); },
        [&](ast::grouped_expr) -> value { return emit_expression_id(id); });
}

auto emitter::emit_array(ast::node_id id, const ast::array_expr& arr) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{active_mod().get_sema_type_opt(id)};
    ASSERT(sema_type, "Array expression must have a resolved sema type");
    if (arr.is_type_expr) { return value{void_val{}, *sema_type}; }

    const auto array_slot{builder_.emit_alloca(*sema_type)};
    auto&      usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};

    const auto arr_data{sema_type->get_data().as_opt<sema::types::array>()};
    ASSERT(arr_data, "Array sema type must contain array type data");
    auto& elem_type{arr_data->underlying};
    for (u64 i{0}; const auto& item : arr.items) {
        const auto elem_ptr{builder_.emit_get_element_ptr(
            value{array_slot, *sema_type}, {value{i++, usize_type}}, elem_type)};
        const auto val{emit_expression(item)};
        builder_.emit_store(value{elem_ptr, elem_type}, val).is_initializer = true;
    }

    const auto loaded{builder_.emit_load(value{array_slot, *sema_type}, *sema_type)};
    return value{loaded, sema_type};
}

auto emitter::emit_ident(ast::node_id id, const ast::identifier_expr& ident) -> value {
    PROFILE_FUNCTION();
    if (const auto binding{lookup_binding(ident.name)}) {
        if (binding->const_val) { return *binding->const_val; }
        if (binding->is_alloca) {
            const auto loaded{builder_.emit_load(binding->id, binding->type)};
            return value{loaded, binding->type};
        }
        return value{binding->id, binding->type};
    }

    // Not a local binding; may be a top-level const/constexpr global, resolvable at compile time
    if (const auto cv{const_eval_.try_eval(id)}) { return materialize_const(*cv); }

    // A module-level / static-member `var` (or otherwise non-foldable) value: load from its global.
    if (const auto gref{try_global_ref(ident.name)}) {
        auto& gtype{const_cast<sema::type&>(*gref->type)};
        return value{builder_.emit_load(*gref, gtype), gtype};
    }
    // A bare `var` sibling static member inside a member fn body.
    if (!user_type_stack_.empty()) {
        if (const auto gref{try_static_member_ref(*user_type_stack_.back(), ident.name)}) {
            auto& gtype{const_cast<sema::type&>(*gref->type)};
            return value{builder_.emit_load(*gref, gtype), gtype};
        }
    }

    const auto sema_type{active_mod().get_sema_type_opt(id)};
    // A bare reference to a sibling `const fn` static member inside a member body lowers to the
    // same function symbol `Type.member` would, not an `undefined` fn value.
    if (sema_type && sema_type->get_kind() == sema::type_kind::FUNCTION) {
        return value{ref_symbol_name(id, ident.name), sema_type};
    }
    return value{undefined_val{}, sema_type};
}

auto emitter::global_ref_in(usize table_idx, std::string_view name) -> stdx::option<value> {
    const auto sym{ctx_.registry.get_from_opt(table_idx, name)};
    if (!sym) { return stdx::none; }
    if (sym->has_kind() && sym->get_kind() != sema::symbol_kind::VALUE) { return stdx::none; }
    const auto node{sym->get_data().as_opt<sema::symbols::node_t>()};
    if (!node) { return stdx::none; }
    const auto decl{active_ast().get_as_opt<ast::decl_stmt>(*node)};
    // Only a `var` value binding has genuine per-program storage that must be shared
    if (!decl || !decl->has_modifier(ast::decl_modifiers::VARIABLE) ||
        decl->has_modifier(ast::decl_modifiers::EXTERN)) {
        return stdx::none;
    }
    const auto raw_ty{active_mod().get_sema_type_opt(*node)};
    if (!raw_ty || raw_ty->get_kind() == sema::type_kind::TYPE ||
        raw_ty->get_kind() == sema::type_kind::FUNCTION) {
        return stdx::none;
    }
    auto&      ty{*ctx_.pool.with_const(*raw_ty, false)};
    const auto addr{
        builder_.emit_global_addr(symbol_scoping_.name_for(table_idx, name), ty, false)};
    return value{addr, ty};
}

auto emitter::try_global_ref(std::string_view name) -> stdx::option<value> {
    const auto rt{active_mod().root_table_idx};
    if (!rt) { return stdx::none; }
    return global_ref_in(*rt, name);
}

auto emitter::try_static_member_ref(const sema::type& owner, std::string_view member)
    -> stdx::option<value> {
    const auto tbl{owner.get_symbol_table_idx_opt()};
    if (!tbl) { return stdx::none; }
    return global_ref_in(*tbl, member);
}

auto emitter::try_emit_union_field_eq(ast::node_id lhs, ast::node_id rhs)
    -> stdx::option<local_id> {
    const auto is_untagged_union = [](const stdx::option<sema::type&>& t) -> bool {
        const auto ut{t ? t->get_data().as_opt<sema::types::union_t>() : stdx::none};
        return !ut || ut->is_untagged;
    };

    const auto lhs_type{active_mod().get_sema_type_opt(lhs)};
    if (!is_untagged_union(lhs_type) && active_ast().get_as_opt<ast::implicit_access_expr>(rhs)) {
        return emit_union_tag_eq(emit_lvalue(lhs), rhs);
    }

    const auto rhs_type{active_mod().get_sema_type_opt(rhs)};
    if (!is_untagged_union(rhs_type) && active_ast().get_as_opt<ast::implicit_access_expr>(lhs)) {
        return emit_union_tag_eq(emit_lvalue(rhs), lhs);
    }

    return stdx::none;
}

// Compares a tagged union's runtime discriminant (index 0) against a `.field` pattern's ordinal.
auto emitter::emit_union_tag_eq(value union_addr, ast::node_id member_pattern_id) -> local_id {
    auto&      i32_type{ctx_.get_int(32, true)};
    auto&      usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
    const auto tag_ptr{builder_.emit_get_element_ptr(
        union_addr, {value{TAGGED_UNION_DISCRIMINANT_INDEX, usize_type}}, i32_type)};
    const auto tag_val{builder_.emit_load(value{tag_ptr, i32_type}, i32_type)};

    const auto& imp{active_ast().get_as<ast::implicit_access_expr>(member_pattern_id)};
    const auto& member_ident{active_ast().get_as<ast::identifier_expr>(imp.member)};
    ASSERT(union_addr.type, "Union tag comparison requires a typed union address");
    const auto& table{ctx_.registry.get(union_addr.type->get_symbol_table_idx())};
    const auto  proxy{table.get_proxy_opt(member_ident.name)};
    ASSERT(proxy, "Union field pattern must reference a valid field");

    auto& bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
    return builder_.emit_binary(instruction_kind::EQ,
                                value{tag_val, i32_type},
                                value{static_cast<i64>(proxy->index), i32_type},
                                bool_type);
}

auto emitter::emit_binary(ast::node_id id, const ast::binary_expr& binary) -> value {
    PROFILE_FUNCTION();
    const auto op_type{id.get_token_type()};
    if (op_type == syntax::token_type_t::BOOLEAN_AND) { return emit_logical_and(id, binary); }
    if (op_type == syntax::token_type_t::BOOLEAN_OR) { return emit_logical_or(id, binary); }

    const auto kind_opt{map_binary_op(op_type)};
    const auto sema_type{active_mod().get_sema_type_opt(id)};
    ASSERT(kind_opt, "Binary operator must be mapped to instruction kind");
    ASSERT(sema_type, "Binary expression must have a resolved sema type");

    if (*kind_opt == instruction_kind::EQ || *kind_opt == instruction_kind::NE) {
        if (const auto tag_eq{try_emit_union_field_eq(binary.lhs, binary.rhs)}) {
            auto& bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
            if (*kind_opt == instruction_kind::NE) {
                return value{builder_.emit_unary(
                                 instruction_kind::NOT, value{*tag_eq, bool_type}, bool_type),
                             sema_type};
            }
            return value{*tag_eq, sema_type};
        }
    }

    const auto lhs{emit_expression(binary.lhs)};
    const auto rhs{emit_expression(binary.rhs)};
    return value{
        emit_checked_binary(
            *kind_opt, lhs, rhs, *sema_type, id, syntax::token_type::is_wrapping_op(op_type)),
        sema_type};
}

auto emitter::emit_unary(ast::node_id id, const ast::unary_expr& unary) -> value {
    PROFILE_FUNCTION();
    const auto op_type{id.get_token_type()};
    const auto kind_opt{map_unary_op(op_type)};
    const auto sema_type{active_mod().get_sema_type_opt(id)};
    ASSERT(kind_opt, "Unary operator must be mapped to instruction kind");
    ASSERT(sema_type, "Unary expression must have a resolved sema type");

    if (const auto cv{const_eval_.try_eval(id)}) { return cv->to_gir_value(); }
    const auto operand{emit_expression(unary.rhs)};
    if (op_type == syntax::token_type_t::BANG && operand.type &&
        operand.type->get_kind() == sema::type_kind::POINTER) {
        return pointer_to_bool(operand, true);
    }
    const auto dest{emit_checked_unary(
        *kind_opt, operand, *sema_type, id, syntax::token_type::is_wrapping_op(op_type))};
    return value{dest, sema_type};
}

// A direct `union.field = ...` write bypasses the initializer's tag, so it's re-synced here.
auto emitter::sync_tagged_union_tag(ast::node_id assign_lhs) -> void {
    const auto dot{active_ast().get_as_opt<ast::dot_expr>(assign_lhs)};
    if (!dot) { return; }

    const auto obj_type{active_mod().get_sema_type_opt(dot->object)};
    if (!obj_type) { return; }
    auto* unwrapped{obj_type.get()};
    if (const auto ptr_data{unwrapped->get_data().as_opt<sema::types::pointer>()}) {
        unwrapped = &ptr_data->underlying;
    } else if (const auto ref_data{unwrapped->get_data().as_opt<sema::types::reference>()}) {
        unwrapped = &ref_data->underlying;
    }
    const auto ut{unwrapped->get_data().as_opt<sema::types::union_t>()};
    if (!ut || ut->is_untagged) { return; }

    const auto& member_ident{active_ast().get_as<ast::identifier_expr>(dot->member)};
    const auto& table{ctx_.registry.get(unwrapped->get_symbol_table_idx())};
    const auto  proxy{table.get_proxy_opt(member_ident.name)};
    ASSERT(proxy, "Union field write must reference a valid field");

    const auto union_addr{emit_lvalue(dot->object)};
    auto&      i32_type{ctx_.get_int(32, true)};
    auto&      usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
    const auto tag_ptr{builder_.emit_get_element_ptr(
        union_addr, {value{TAGGED_UNION_DISCRIMINANT_INDEX, usize_type}}, i32_type)};
    // The tag write establishes the active variant, the same as a construction-time write does
    builder_.emit_store(value{tag_ptr, i32_type}, value{static_cast<i64>(proxy->index), i32_type})
        .is_initializer = true;
}

auto emitter::emit_packed_field_read(value                        backing_addr,
                                     const sema::types::struct_t& st,
                                     usize                        field_idx,
                                     sema::type&                  field_type) -> value {
    const auto  n{sema::packed_backing_bits(st, target_ptr_bits_).value_or(1)};
    auto&       backing_ty{ctx_.get_int(static_cast<u16>(n), false)};
    const value backing{builder_.emit_load(backing_addr, backing_ty), backing_ty};
    return emit_packed_field_extract(backing, st, field_idx, field_type);
}

// Slice one field's value out of an already-loaded `packed struct` backing integer.
auto emitter::emit_packed_field_extract(value                        backing,
                                        const sema::types::struct_t& st,
                                        usize                        field_idx,
                                        sema::type&                  field_type) -> value {
    const auto n{sema::packed_backing_bits(st, target_ptr_bits_).value_or(1)};
    const auto offset{sema::packed_field_offset(st, field_idx, target_ptr_bits_)};
    const auto fbits{sema::packed_field_bits(field_type, target_ptr_bits_).value_or(n)};
    return emit_packed_bits_extract(backing, n, offset, fbits, field_type);
}

// Every field of a bit-packed `packed union` sits at bit offset 0.
auto emitter::emit_packed_field_read(value                       backing_addr,
                                     const sema::types::union_t& ut,
                                     sema::type&                 field_type) -> value {
    const auto  n{sema::packed_union_backing_bits(ut, target_ptr_bits_).value_or(1)};
    const auto  fbits{sema::packed_field_bits(field_type, target_ptr_bits_).value_or(n)};
    auto&       backing_ty{ctx_.get_int(static_cast<u16>(n), false)};
    const value backing{builder_.emit_load(backing_addr, backing_ty), backing_ty};
    return emit_packed_bits_extract(backing, n, 0, fbits, field_type);
}

auto emitter::emit_packed_field_write(value                       backing_addr,
                                      const sema::types::union_t& ut,
                                      sema::type&                 field_type,
                                      value                       new_field_val) -> void {
    const auto n{sema::packed_union_backing_bits(ut, target_ptr_bits_).value_or(1)};
    const auto fbits{sema::packed_field_bits(field_type, target_ptr_bits_).value_or(n)};
    emit_packed_bits_insert(backing_addr, n, 0, fbits, field_type, new_field_val);
}

// Slice `fbits` bits starting at `offset` out of `backing` and reinterpret them as `field_type`
auto emitter::emit_packed_bits_extract(
    value backing, u32 n, u32 offset, u32 fbits, sema::type& field_type) -> value {
    auto& backing_ty{ctx_.get_int(static_cast<u16>(n), false)};

    const value shifted{
        offset == 0
            ? backing
            : value{builder_.emit_binary(
                        instruction_kind::SHR, backing, value{u64{offset}, backing_ty}, backing_ty),
                    backing_ty}};

    const auto kind{field_type.get_kind()};
    if (kind == sema::type_kind::INT) {
        return value{builder_.emit_cast(instruction_kind::WIDEN_CAST, shifted, field_type),
                     field_type};
    }

    auto&       fbits_uint{ctx_.get_int(static_cast<u16>(fbits), false)};
    const value narrowed{builder_.emit_cast(instruction_kind::WIDEN_CAST, shifted, fbits_uint),
                         fbits_uint};
    if (sema::is_float(kind) || kind == sema::type_kind::BOOL || kind == sema::type_kind::STRUCT ||
        kind == sema::type_kind::UNION) {
        // `bool` is `i1`, a nested `packed` aggregate is its own `iN`: reinterpret the bits.
        return value{builder_.emit_cast(instruction_kind::BIT_CAST, narrowed, field_type),
                     field_type};
    }
    if (kind == sema::type_kind::POINTER || kind == sema::type_kind::REFERENCE) {
        return value{builder_.emit_cast(instruction_kind::PTR_FROM_INT, narrowed, field_type),
                     field_type};
    }
    // enum / usize / isize: the field-width integer is the value.
    return value{builder_.emit_cast(instruction_kind::WIDEN_CAST, narrowed, field_type),
                 field_type};
}

auto emitter::emit_packed_field_write(value                        backing_addr,
                                      const sema::types::struct_t& st,
                                      usize                        field_idx,
                                      sema::type&                  field_type,
                                      value                        new_field_val) -> void {
    const auto n{sema::packed_backing_bits(st, target_ptr_bits_).value_or(1)};
    const auto offset{sema::packed_field_offset(st, field_idx, target_ptr_bits_)};
    const auto fbits{sema::packed_field_bits(field_type, target_ptr_bits_).value_or(n)};
    emit_packed_bits_insert(backing_addr, n, offset, fbits, field_type, new_field_val);
}

// Read-modify-write `fbits` bits at `offset` of the `n`-bit backing integer at `backing_addr`
// with `new_field_val` reinterpreted from `field_type`.
auto emitter::emit_packed_bits_insert(
    value backing_addr, u32 n, u32 offset, u32 fbits, sema::type& field_type, value new_field_val)
    -> void {
    auto&       backing_ty{ctx_.get_int(static_cast<u16>(n), false)};
    const value old_backing{builder_.emit_load(backing_addr, backing_ty), backing_ty};
    const value merged{
        emit_packed_bits_merge(old_backing, n, offset, fbits, field_type, new_field_val)};
    builder_.emit_store(backing_addr, merged);
}

// The `n`-bit backing integer that is `old_backing` with `fbits` bits at
// `offset` replaced by `new_field_val` reinterpreted from `field_type` (for nested writes)
auto emitter::emit_packed_bits_merge(
    value old_backing, u32 n, u32 offset, u32 fbits, sema::type& field_type, value new_field_val)
    -> value {
    auto& backing_ty{ctx_.get_int(static_cast<u16>(n), false)};
    auto& fbits_uint{ctx_.get_int(static_cast<u16>(fbits), false)};

    const auto kind{field_type.get_kind()};
    value      as_fbits{new_field_val};
    if (sema::is_float(kind) || kind == sema::type_kind::BOOL || kind == sema::type_kind::STRUCT ||
        kind == sema::type_kind::UNION) {
        // `bool` is `i1`, a nested `packed` aggregate is its own `iN`: reinterpret the bits.
        as_fbits = value{builder_.emit_cast(instruction_kind::BIT_CAST, new_field_val, fbits_uint),
                         fbits_uint};
    } else if (kind == sema::type_kind::POINTER || kind == sema::type_kind::REFERENCE) {
        as_fbits =
            value{builder_.emit_cast(instruction_kind::INT_FROM_PTR, new_field_val, fbits_uint),
                  fbits_uint};
    } else {
        as_fbits =
            value{builder_.emit_cast(instruction_kind::WIDEN_CAST, new_field_val, fbits_uint),
                  fbits_uint};
    }

    const value zexted{builder_.emit_cast(instruction_kind::WIDEN_CAST, as_fbits, backing_ty),
                       backing_ty};
    const value shifted{
        offset == 0
            ? zexted
            : value{builder_.emit_binary(
                        instruction_kind::SHL, zexted, value{u64{offset}, backing_ty}, backing_ty),
                    backing_ty}};

    const u128 field_mask{fbits >= 128 ? ~u128{0} : ((u128{1} << fbits) - 1)};
    const u128 all_ones{n >= 128 ? ~u128{0} : ((u128{1} << n) - 1)};
    const u128 mask{(field_mask << offset) & all_ones};
    const u128 inv_mask{all_ones ^ mask};

    const value bits_in{
        builder_.emit_binary(instruction_kind::AND, shifted, value{mask, backing_ty}, backing_ty),
        backing_ty};
    const value cleared{
        builder_.emit_binary(
            instruction_kind::AND, old_backing, value{inv_mask, backing_ty}, backing_ty),
        backing_ty};
    return value{builder_.emit_binary(instruction_kind::OR, cleared, bits_in, backing_ty),
                 backing_ty};
}

namespace {

// Peels one `^`/`&` layer off `t`.
[[nodiscard]] auto peel_ptr_ref(const sema::type& t) -> const sema::type& {
    if (const auto p{t.get_data().as_opt<sema::types::pointer>()}) { return p->underlying; }
    if (const auto r{t.get_data().as_opt<sema::types::reference>()}) { return r->underlying; }
    return t;
}

[[nodiscard]] auto is_bit_packed_agg(const sema::type& t) -> bool {
    if (const auto st{t.get_data().as_opt<sema::types::struct_t>()}) { return st->is_bit_packed(); }
    if (const auto ut{t.get_data().as_opt<sema::types::union_t>()}) { return ut->is_bit_packed(); }
    return false;
}

} // namespace

// The bit layout of `dot`'s field within its (bit-packed) enclosing aggregate.
auto emitter::packed_layout_of(const ast::dot_expr& dot) -> packed_field_layout {
    const auto& obj_ty{peel_ptr_ref(*active_mod().get_sema_type_opt(dot.object))};
    const auto& member_ident{active_ast().get_as<ast::identifier_expr>(dot.member)};
    const auto& table{ctx_.registry.get(obj_ty.get_symbol_table_idx())};
    const auto  field_idx{table.get_proxy(member_ident.name).index};
    auto&       field_type{active_mod()
                         .get_sema_type_opt(dot.member)
                         .value_or(*active_mod().get_sema_type_opt(dot.object))};

    if (const auto st{obj_ty.get_data().as_opt<sema::types::struct_t>()}) {
        const auto n{sema::packed_backing_bits(*st, target_ptr_bits_).value_or(1)};
        return {
            .n          = n,
            .offset     = sema::packed_field_offset(*st, field_idx, target_ptr_bits_),
            .fbits      = sema::packed_field_bits(field_type, target_ptr_bits_).value_or(n),
            .field_idx  = field_idx,
            .field_type = &field_type,
        };
    }
    const auto& ut{obj_ty.get_data().as<sema::types::union_t>()};
    const auto  n{sema::packed_union_backing_bits(ut, target_ptr_bits_).value_or(1)};
    return {
        .n          = n,
        .offset     = 0,
        .fbits      = sema::packed_field_bits(field_type, target_ptr_bits_).value_or(n),
        .field_idx  = field_idx,
        .field_type = &field_type,
    };
}

auto emitter::emit_packed_store(const ast::dot_expr& dot, value field_val) -> void {
    const auto layout{packed_layout_of(dot)};

    const auto inner{active_ast().get_as_opt<ast::dot_expr>(dot.object)};
    if (inner && is_bit_packed_agg(peel_ptr_ref(*active_mod().get_sema_type_opt(inner->object)))) {
        auto&       backing_ty{ctx_.get_int(static_cast<u16>(layout.n), false)};
        const value old_obj{emit_expression(dot.object).data, backing_ty};
        const value new_obj{emit_packed_bits_merge(
            old_obj, layout.n, layout.offset, layout.fbits, *layout.field_type, field_val)};
        emit_packed_store(*inner, new_obj);
        return;
    }

    auto base_lval{emit_lvalue(dot.object)};
    if (const auto obj_t{active_mod().get_sema_type_opt(dot.object)}) {
        if (obj_t->get_data().as_opt<sema::types::pointer>() ||
            obj_t->get_data().as_opt<sema::types::reference>()) {
            base_lval.data = value::data_t{builder_.emit_load(base_lval, *obj_t)};
        }
    }
    emit_packed_bits_insert(
        base_lval, layout.n, layout.offset, layout.fbits, *layout.field_type, field_val);
}

auto emitter::emit_packed_field_assign(ast::node_id                id,
                                       const ast::dot_expr&        dot,
                                       const ast::assignment_expr& assign,
                                       syntax::token_type_t        op_type) -> value {
    PROFILE_FUNCTION();
    const auto layout{packed_layout_of(dot)};
    auto&      field_type{*layout.field_type};

    value new_field{};
    if (op_type == syntax::token_type_t::ASSIGN) {
        new_field = emit_coerced_expr(assign.rhs, field_type);
    } else {
        auto&       backing_ty{ctx_.get_int(static_cast<u16>(layout.n), false)};
        const value backing{emit_expression(dot.object).data, backing_ty};
        const auto  old_field{
            emit_packed_bits_extract(backing, layout.n, layout.offset, layout.fbits, field_type)};
        auto base_tok{op_type};
        if (const auto b{syntax::token_type::get_compound_base_op(op_type)}) { base_tok = *b; }
        const auto base_kind{map_binary_op(base_tok).value_or(instruction_kind::ADD)};
        const auto rhs{emit_expression(assign.rhs)};
        new_field = value{emit_checked_binary(base_kind,
                                              old_field,
                                              rhs,
                                              field_type,
                                              id,
                                              syntax::token_type::is_wrapping_op(base_tok)),
                          field_type};
    }

    emit_packed_store(dot, new_field);
    return new_field;
}

auto emitter::emit_assignment(ast::node_id id, const ast::assignment_expr& assign) -> value {
    PROFILE_FUNCTION();
    const auto op_type{id.get_token_type()};
    const auto sema_type{active_mod().get_sema_type_opt(id)};
    ASSERT(sema_type, "Assignment expression must have a resolved sema type");

    // A field write on a bit-packed struct is a read-modify-write of the backing integer.
    if (const auto dot{active_ast().get_as_opt<ast::dot_expr>(assign.lhs)}) {
        if (const auto ot{active_mod().get_sema_type_opt(dot->object)}) {
            const sema::type* u{ot.get()};
            if (const auto p{u->get_data().as_opt<sema::types::pointer>()}) {
                u = &p->underlying;
            } else if (const auto r{u->get_data().as_opt<sema::types::reference>()}) {
                u = &r->underlying;
            }
            if (is_bit_packed_agg(*u)) {
                return emit_packed_field_assign(id, *dot, assign, op_type);
            }
        }
    }

    sync_tagged_union_tag(assign.lhs);
    auto lhs_lval{emit_lvalue(assign.lhs)};
    ASSERT(lhs_lval.type, "Assignment LHS must have a resolved type");

    // A reference-typed assignment target has value semantics
    if (const auto ref_data{lhs_lval.type->get_data().as_opt<sema::types::reference>()}) {
        auto& referent_type{const_cast<sema::type&>(ref_data->underlying)};
        lhs_lval.data = value::data_t{builder_.emit_load(lhs_lval, *lhs_lval.type)};
        lhs_lval.type.emplace(referent_type);
    }

    if (op_type == syntax::token_type_t::ASSIGN) {
        const value rhs{emit_coerced_expr(assign.rhs, *lhs_lval.type)};
        builder_.emit_store(lhs_lval, rhs);
        return rhs;
    }

    switch (op_type) {
    case syntax::token_type_t::PLUS_ASSIGN:
    case syntax::token_type_t::PLUS_PERCENT_ASSIGN:
    case syntax::token_type_t::MINUS_ASSIGN:
    case syntax::token_type_t::MINUS_PERCENT_ASSIGN:
    case syntax::token_type_t::STAR_ASSIGN:
    case syntax::token_type_t::STAR_PERCENT_ASSIGN:
    case syntax::token_type_t::SLASH_ASSIGN:
    case syntax::token_type_t::PERCENT_ASSIGN:
    case syntax::token_type_t::BW_AND_ASSIGN:
    case syntax::token_type_t::BW_OR_ASSIGN:
    case syntax::token_type_t::XOR_ASSIGN:
    case syntax::token_type_t::SHL_ASSIGN:
    case syntax::token_type_t::SHL_PERCENT_ASSIGN:
    case syntax::token_type_t::SHR_ASSIGN:           {
        auto base_tok{op_type};
        if (const auto b{syntax::token_type::get_compound_base_op(op_type)}) { base_tok = *b; }
        const auto base_kind{map_binary_op(base_tok).value_or(instruction_kind::ADD)};
        auto&      target_type{*lhs_lval.type};
        const auto loaded{builder_.emit_load(lhs_lval, target_type)};
        const auto rhs{emit_expression(assign.rhs)};
        const auto res_val{value{emit_checked_binary(base_kind,
                                                     value{loaded, target_type},
                                                     rhs,
                                                     target_type,
                                                     id,
                                                     syntax::token_type::is_wrapping_op(base_tok)),
                                 target_type}};
        builder_.emit_store(lhs_lval, res_val);
        return res_val;
    }
    default: UNREACHABLE("Unhandled assignment operator type");
    }
}

auto emitter::emit_asm(ast::node_id id, const ast::asm_expr& node) -> value {
    PROFILE_FUNCTION();
    auto& void_type{ctx_.get_builtin_resolved_type(sema::type_kind::VOID_)};

    const auto constraint_text = [&](const ast::string_handle& h) -> std::string_view {
        return active_ast().get_as<ast::string_expr>(h).value;
    };

    inline_asm info;
    info.is_volatile   = node.has_option(ast::asm_expr::option::VOLATILE);
    info.is_noreturn   = node.has_option(ast::asm_expr::option::NORETURN);
    info.align_stack   = node.has_option(ast::asm_expr::option::ALIGN_STACK);
    info.intel_dialect = node.has_option(ast::asm_expr::option::INTEL);
    info.tmpl = rewrite_asm_template(active_ast().get_as<ast::string_expr>(node.tmpl).value);

    // Constraint list: outputs, then inputs, then clobbers
    std::vector<std::string> pieces;
    for (const auto& op : node.outputs) { pieces.emplace_back(constraint_text(op.constraint)); }

    std::vector<value> inputs;
    inputs.reserve(node.inputs.size());
    for (const auto& op : node.inputs) {
        pieces.emplace_back(constraint_text(op.constraint));
        ASSERT(op.value, "asm input operand must carry an expression");
        inputs.emplace_back(emit_expression(*op.value));
    }
    for (const auto& clob : node.clobbers) {
        pieces.emplace_back(fmt::format("~{{{}}}", constraint_text(clob)));
    }
    for (usize i{0}; i < pieces.size(); ++i) {
        if (i != 0) { info.constraints += ','; }
        info.constraints += pieces[i];
    }

    // Bind each output: a real lvalue gets a store target, `_` feeds the asm's own result.
    stdx::option<sema::type&> result_type;
    for (const auto& op : node.outputs) {
        if (op.is_result_slot()) {
            info.has_result_slot = true;
            result_type          = active_mod().get_sema_type_opt(id);
        } else {
            info.output_addrs.emplace_back(emit_lvalue(*op.value));
        }
    }

    auto&      asm_result_type{result_type ? *result_type : void_type};
    const auto dest{builder_.emit_inline_asm(std::move(info), std::move(inputs), asm_result_type)};

    if (node.has_option(ast::asm_expr::option::NORETURN)) {
        builder_.emit_unreachable();
        return value{undefined_val{}, ctx_.get_builtin_resolved_type(sema::type_kind::NORETURN)};
    }
    if (dest) { return value{*dest, asm_result_type}; }
    return value{void_val{}, void_type};
}

auto emitter::emit_call(ast::node_id id, const ast::call_expr& call) -> value {
    PROFILE_FUNCTION();
    auto& ret_type{active_mod().get_sema_type_opt(id).value_or(
        ctx_.get_builtin_resolved_type(sema::type_kind::VOID_))};

    // Builtin call handling
    const auto fn_token{call.function->get_token_type()};
    if (syntax::get_builtin_opt(fn_token)) {
        switch (fn_token) {
        case syntax::token_type_t::BUILTIN_AS:
        case syntax::token_type_t::BUILTIN_BIT_CAST:
        case syntax::token_type_t::BUILTIN_PTR_CAST:
        case syntax::token_type_t::BUILTIN_ALIGN_CAST: {
            if (call.arguments.size() >= 2) {
                if (const auto op_expr{call.arguments[1].as_opt<ast::expr_handle>()}) {
                    const auto operand{emit_expression(*op_expr)};

                    // `@as(bool, x)` tests a pointer or integer against its zero value.
                    if (fn_token == syntax::token_type_t::BUILTIN_AS &&
                        ret_type.get_kind() == sema::type_kind::BOOL && operand.type) {
                        const auto operand_kind{operand.type->get_kind()};
                        if (operand_kind == sema::type_kind::POINTER) {
                            return pointer_to_bool(operand, false);
                        }
                        if (sema::is_integer(operand_kind) ||
                            sema::is_constexpr_int(operand_kind)) {
                            auto& bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
                            return value{builder_.emit_binary(instruction_kind::NE,
                                                              operand,
                                                              value{u64{0}, *operand.type},
                                                              bool_type),
                                         bool_type};
                        }
                    }

                    auto cast_kind{instruction_kind::WIDEN_CAST};
                    if (fn_token == syntax::token_type_t::BUILTIN_BIT_CAST) {
                        cast_kind = instruction_kind::BIT_CAST;
                    } else if (fn_token == syntax::token_type_t::BUILTIN_PTR_CAST ||
                               fn_token == syntax::token_type_t::BUILTIN_ALIGN_CAST) {
                        cast_kind = instruction_kind::PTR_CAST;
                    }
                    const auto dest{builder_.emit_cast(cast_kind, operand, ret_type)};
                    value      result{dest, ret_type};
                    emit_enum_cast_guard(id, result, operand, *op_expr);
                    return result;
                }
            }
            break;
        }
        case syntax::token_type_t::BUILTIN_PTR_FROM_INT: {
            if (call.arguments.size() >= 2) {
                if (const auto op_expr{call.arguments[1].as_opt<ast::expr_handle>()}) {
                    const auto operand{emit_expression(*op_expr)};
                    const auto dest{
                        builder_.emit_cast(instruction_kind::PTR_FROM_INT, operand, ret_type)};
                    return value{dest, ret_type};
                }
            }
            break;
        }
        case syntax::token_type_t::BUILTIN_DYN_CAST: {
            // `@dynCast(^T, w)` -> `w`'s erased `data` pointer, retyped. No RTTI check.
            if (call.arguments.size() >= 2) {
                if (const auto op_expr{call.arguments[1].as_opt<ast::expr_handle>()}) {
                    const auto        src_ty{active_mod().get_sema_type_opt(*op_expr)};
                    const sema::type* dyn_ty{nullptr};
                    if (src_ty) {
                        if (const auto p{src_ty->get_data().as_opt<sema::types::pointer>()}) {
                            dyn_ty = &p->underlying;
                        } else if (const auto r{
                                       src_ty->get_data().as_opt<sema::types::reference>()}) {
                            dyn_ty = &r->underlying;
                        }
                    }
                    if (!dyn_ty) { break; }
                    auto& dyn_mut{const_cast<sema::type&>(*dyn_ty)};
                    auto& ptr_ty{
                        ctx_.get_pointer(sema::types::mut::CONSTANT,
                                         ctx_.get_builtin_resolved_type(sema::type_kind::OPAQUE))};
                    auto&      usize_ty{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
                    const auto fat{value{emit_expression_id_raw(*op_expr).data, dyn_mut}};
                    const auto slot{builder_.emit_alloca(dyn_mut)};
                    builder_.emit_store(value{slot, dyn_mut}, fat);
                    const auto d_ptr{builder_.emit_get_element_ptr(
                        value{slot, dyn_mut}, {value{u64{0}, usize_ty}}, ptr_ty)};
                    return value{builder_.emit_load(value{d_ptr, ptr_ty}, ret_type), ret_type};
                }
            }
            break;
        }
        case syntax::token_type_t::BUILTIN_INT_FROM_PTR: {
            if (!call.arguments.empty()) {
                if (const auto op_expr{call.arguments[0].as_opt<ast::expr_handle>()}) {
                    const auto operand{emit_expression(*op_expr)};
                    const auto dest{
                        builder_.emit_cast(instruction_kind::INT_FROM_PTR, operand, ret_type)};
                    return value{dest, ret_type};
                }
            }
            break;
        }
        case syntax::token_type_t::BUILTIN_CONST_CAST:
        case syntax::token_type_t::BUILTIN_VOLATILE_CAST: {
            if (!call.arguments.empty()) {
                if (const auto op_expr{call.arguments[0].as_opt<ast::expr_handle>()}) {
                    const auto operand{emit_expression(*op_expr)};
                    return value{operand.data, ret_type};
                }
            }
            break;
        }
        case syntax::token_type_t::BUILTIN_PTR_FROM_ARRAY: {
            if (!call.arguments.empty()) {
                if (const auto op_expr{call.arguments[0].as_opt<ast::expr_handle>()}) {
                    const auto base_lval{emit_lvalue(*op_expr)};
                    auto&      usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
                    const auto elem_ptr{builder_.emit_get_element_ptr(
                        base_lval, {value{static_cast<u64>(0), usize_type}}, ret_type)};
                    return value{elem_ptr, ret_type};
                }
            }
            break;
        }
        case syntax::token_type_t::BUILTIN_SLICE_FROM_PTR: {
            if (call.arguments.size() >= 2) {
                const auto op_ptr{call.arguments[0].as_opt<ast::expr_handle>()};
                const auto op_len{call.arguments[1].as_opt<ast::expr_handle>()};
                if (op_ptr && op_len) {
                    const auto ptr_val{emit_expression(*op_ptr)};
                    const auto len_val{emit_expression(*op_len)};
                    auto&      usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
                    const auto slice_slot{builder_.emit_alloca(ret_type)};
                    auto&      ptr_type{ptr_val.type.value_or(ret_type)};
                    const auto field0{
                        builder_.emit_get_element_ptr(value{slice_slot, ret_type},
                                                      {value{SLICE_PTR_FIELD_INDEX, usize_type}},
                                                      ptr_type)};
                    builder_.emit_store(value{field0, ptr_type}, ptr_val).is_initializer = true;
                    const auto field1{
                        builder_.emit_get_element_ptr(value{slice_slot, ret_type},
                                                      {value{SLICE_LEN_FIELD_INDEX, usize_type}},
                                                      usize_type)};
                    builder_.emit_store(value{field1, usize_type}, len_val).is_initializer = true;
                    return value{builder_.emit_load(value{slice_slot, ret_type}, ret_type),
                                 ret_type};
                }
            }
            break;
        }
        case syntax::token_type_t::BUILTIN_FIELD_PARENT_PTR: {
            const auto name_h{call.arguments[1].as_opt<ast::expr_handle>()};
            const auto ptr_h{call.arguments[2].as_opt<ast::expr_handle>()};
            const auto parent_ptr{ret_type.get_data().as_opt<sema::types::pointer>()};
            if (name_h && ptr_h && parent_ptr) {
                const auto& field_name{active_ast().get_as<ast::string_expr>(*name_h).value};
                const auto& table{ctx_.registry.get(parent_ptr->underlying.get_symbol_table_idx())};
                const auto  field_idx{table.get_proxy(field_name).index};
                auto&       usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};

                std::vector<value> args;
                args.emplace_back(emit_expression(*ptr_h));
                args.emplace_back(value{static_cast<u64>(field_idx), usize_type});
                if (const auto res{
                        builder_.emit_builtin_call("@fieldParentPtr", std::move(args), ret_type)}) {
                    return value{*res, ret_type};
                }
            }
            break;
        }
        case syntax::token_type_t::BUILTIN_PANIC: {
            // The resolver has already checked the message is a compile-time-constant string.
            std::string message{"panic"};
            if (!call.arguments.empty()) {
                if (const auto expr_h{call.arguments[0].as_opt<ast::expr_handle>()}) {
                    if (const auto str_expr{active_ast().get_as_opt<ast::string_expr>(*expr_h)}) {
                        message = std::string{str_expr->value};
                    } else if (const auto cv{const_eval_.try_eval(*expr_h)}) {
                        if (const auto s{cv->as_opt<std::string>()}) { message = *s; }
                    }
                }
            }
            emit_panic_call(message, id);
            return value{void_val{}, ret_type};
        }
        case syntax::token_type_t::BUILTIN_TRAP: {
            builder_.emit_builtin_call("@trap", {}, ret_type);
            builder_.emit_unreachable();
            return value{void_val{}, ret_type};
        }
        case syntax::token_type_t::BUILTIN_SRC: {
            // `@src()` yields a `builtin::SourceLocation` aggregate
            const auto         loc{active_ast().location_of(id)};
            auto&              u32_type{ctx_.get_int(32, false)};
            std::vector<value> args;
            args.emplace_back(
                const_value::make_string(ctx_, active_mod().path.string()).to_gir_value());
            args.emplace_back(value{static_cast<u64>(loc.line), u32_type});
            args.emplace_back(value{static_cast<u64>(loc.column), u32_type});
            if (const auto res{builder_.emit_builtin_call("@src", std::move(args), ret_type)}) {
                return value{*res, ret_type};
            }
            return value{void_val{}, ret_type};
        }
        case syntax::token_type_t::BUILTIN_SIZE_OF:
        case syntax::token_type_t::BUILTIN_BIT_SIZE_OF:
        case syntax::token_type_t::BUILTIN_ALIGN_OF:
        case syntax::token_type_t::BUILTIN_TYPE_OF:
        case syntax::token_type_t::BUILTIN_TYPE_NAME:
        case syntax::token_type_t::BUILTIN_TARGET_OS:
        case syntax::token_type_t::BUILTIN_TARGET_ARCH:
        case syntax::token_type_t::BUILTIN_TARGET_TRIPLE: {
            if (const auto cv{const_eval_.try_eval(id)}) { return cv->to_gir_value(); }
            break;
        }
        case syntax::token_type_t::BUILTIN_TAG_NAME: {
            if (const auto cv{const_eval_.try_eval(id)}) {
                // A bare string constant's `to_gir_value()` carries only a data pointer
                if (const auto s{cv->as_opt<std::string>()}) {
                    return materialize_string_slice(*s, ret_type);
                }
                return cv->to_gir_value();
            }
            if (!call.arguments.empty()) {
                if (const auto expr_h{call.arguments.front().as_opt<ast::expr_handle>()}) {
                    if (auto operand_type{active_mod().get_sema_type_opt(*expr_h)}) {
                        if (const auto res{
                                emit_runtime_tag_name(*expr_h, *operand_type, ret_type)}) {
                            return *res;
                        }
                    }
                }
            }
            break;
        }
        case syntax::token_type_t::BUILTIN_EXPECT:
        case syntax::token_type_t::BUILTIN_REQUIRE: {
            const auto is_expect{fn_token == syntax::token_type_t::BUILTIN_EXPECT};

            value               cond_val{void_val{}, ret_type};
            stdx::option<value> msg_val;
            bool                first{true};
            for (const auto& arg : call.arguments) {
                if (const auto expr_h{arg.as_opt<ast::expr_handle>()}) {
                    if (first) {
                        cond_val = emit_expression(*expr_h);
                        first    = false;
                    } else {
                        msg_val.emplace(emit_expression(*expr_h));
                    }
                }
            }

            const auto loc{active_ast().location_of(id)};
            auto&      u32_type{ctx_.get_int(32, false)};

            std::vector<value> args;
            args.emplace_back(std::move(cond_val));
            args.emplace_back(
                const_value::make_string(ctx_, active_mod().path.string()).to_gir_value());
            args.emplace_back(value{static_cast<u64>(loc.line), u32_type});
            args.emplace_back(value{static_cast<u64>(loc.column), u32_type});
            args.emplace_back(msg_val ? std::move(*msg_val)
                                      : const_value::make_string(ctx_, "").to_gir_value());

            // The failure branch calls a weak context handler
            request_builtin_runtime(is_expect ? "expect_handler" : "require_handler");

            const auto* name{is_expect ? "@expect" : "@require"};
            const auto  local_res{builder_.emit_builtin_call(name, std::move(args), ret_type)};
            if (is_expect && local_res) { return value{*local_res, ret_type}; }
            return value{void_val{}, ret_type};
        }
        case syntax::token_type_t::BUILTIN_ASSERT:
        case syntax::token_type_t::BUILTIN_VERIFY: {
            const bool is_verify{fn_token == syntax::token_type_t::BUILTIN_VERIFY};

            // A comptime-known-true condition needs no check
            if (!call.arguments.empty()) {
                if (const auto ch{call.arguments[0].as_opt<ast::expr_handle>()}) {
                    if (const auto cv{const_eval_.try_eval(*ch)}) {
                        if (const auto b{cv->as_opt<bool>()}; b && *b) {
                            return value{void_val{}, ret_type};
                        }
                    }
                }
            }
            if (!is_verify && !runtime_safety_) { return value{void_val{}, ret_type}; }

            std::string message{is_verify ? "verification failed" : "assertion failed"};
            if (call.arguments.size() > 1) {
                if (const auto mh{call.arguments[1].as_opt<ast::expr_handle>()}) {
                    if (const auto se{active_ast().get_as_opt<ast::string_expr>(*mh)}) {
                        message = std::string{se->value};
                    } else if (const auto mv{const_eval_.try_eval(*mh)}) {
                        if (const auto s{mv->as_opt<std::string>()}) { message = *s; }
                    }
                }
            }

            const auto loc{active_ast().location_of(id)};
            auto&      u32_type{ctx_.get_int(32, false)};

            std::vector<value> args;
            args.emplace_back(emit_expression(*call.arguments[0].as_opt<ast::expr_handle>()));
            args.emplace_back(
                const_value::make_string(ctx_, active_mod().path.string()).to_gir_value());
            args.emplace_back(value{static_cast<u64>(loc.line), u32_type});
            args.emplace_back(value{static_cast<u64>(loc.column), u32_type});
            args.emplace_back(const_value::make_string(ctx_, message).to_gir_value());

            request_builtin_runtime(is_verify ? "panic_handler" : "assert_handler");
            builder_.emit_builtin_call(
                is_verify ? "@verify" : "@assert", std::move(args), ret_type);
            return value{void_val{}, ret_type};
        }
        case syntax::token_type_t::BUILTIN_SKIP: {
            // `@skip(["msg"])` marks the enclosing test as skipped and returns from it.
            stdx::option<value> msg_val;
            for (const auto& arg : call.arguments) {
                if (const auto expr_h{arg.as_opt<ast::expr_handle>()}) {
                    msg_val.emplace(emit_expression(*expr_h));
                }
            }

            const auto loc{active_ast().location_of(id)};
            auto&      u32_type{ctx_.get_int(32, false)};
            auto&      void_type{ctx_.get_builtin_resolved_type(sema::type_kind::VOID_)};
            auto&      bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};

            std::vector<value> args;
            args.emplace_back(msg_val ? std::move(*msg_val)
                                      : const_value::make_string(ctx_, "").to_gir_value());
            args.emplace_back(
                const_value::make_string(ctx_, active_mod().path.string()).to_gir_value());
            args.emplace_back(value{static_cast<u64>(loc.line), u32_type});
            args.emplace_back(value{static_cast<u64>(loc.column), u32_type});

            request_builtin_runtime("skip_handler");
            builder_.emit_builtin_call("@skip", std::move(args), void_type);
            // A skipped test simply passes: return `true` and stop emitting this path.
            builder_.emit_return(value{true, bool_type});
            return value{void_val{}, ret_type};
        }
        case syntax::token_type_t::BUILTIN_CLZ:
        case syntax::token_type_t::BUILTIN_CTZ:
        case syntax::token_type_t::BUILTIN_POP_COUNT:
        case syntax::token_type_t::BUILTIN_ABS:
        case syntax::token_type_t::BUILTIN_MIN:
        case syntax::token_type_t::BUILTIN_MAX:
        case syntax::token_type_t::BUILTIN_DIV_TRUNC:
        case syntax::token_type_t::BUILTIN_DIV_FLOOR:
        case syntax::token_type_t::BUILTIN_REM:
        case syntax::token_type_t::BUILTIN_MOD:       {
            if (const auto cv{const_eval_.try_eval(id)}) { return cv->to_gir_value(); }

            std::vector<value> args;
            args.reserve(call.arguments.size());
            for (const auto& arg : call.arguments) {
                if (const auto expr_h{arg.as_opt<ast::expr_handle>()}) {
                    args.emplace_back(emit_expression(*expr_h));
                }
            }
            const auto name{*syntax::get_builtin_opt(fn_token)};
            if (const auto res{builder_.emit_builtin_call(name, std::move(args), ret_type)}) {
                return value{*res, ret_type};
            }
            return value{void_val{}, ret_type};
        }
        case syntax::token_type_t::BUILTIN_ADD_WITH_OVERFLOW:
        case syntax::token_type_t::BUILTIN_SUB_WITH_OVERFLOW:
        case syntax::token_type_t::BUILTIN_MUL_WITH_OVERFLOW:
        case syntax::token_type_t::BUILTIN_SHL_WITH_OVERFLOW: {
            // args: a, b, and a `&mut T` / `^mut T` result slot passed as its raw address.
            std::vector<value> args;
            args.emplace_back(emit_expression(*call.arguments[0].as_opt<ast::expr_handle>()));
            args.emplace_back(emit_expression(*call.arguments[1].as_opt<ast::expr_handle>()));
            args.emplace_back(
                emit_expression_id_raw(*call.arguments[2].as_opt<ast::expr_handle>()));
            const auto name{*syntax::get_builtin_opt(fn_token)};
            if (const auto res{builder_.emit_builtin_call(name, std::move(args), ret_type)}) {
                return value{*res, ret_type};
            }
            return value{void_val{}, ret_type};
        }
        case syntax::token_type_t::BUILTIN_ATOMIC_LOAD:
        case syntax::token_type_t::BUILTIN_ATOMIC_STORE:
        case syntax::token_type_t::BUILTIN_ATOMIC_RMW:
        case syntax::token_type_t::BUILTIN_CMPXCHG_WEAK:
        case syntax::token_type_t::BUILTIN_CMPXCHG_STRONG:
        case syntax::token_type_t::BUILTIN_FENCE:          {
            const auto name{*syntax::get_builtin_opt(fn_token)};

            // The order/op arguments are compile-time enum constants, sema-verified already
            const auto eval_order = [&](usize arg_idx) -> u8 {
                const auto expr_h{*call.arguments[arg_idx].as_opt<ast::expr_handle>()};
                const auto val{const_eval_.try_eval(expr_h)};
                const auto en{val ? val->as_opt<const_enum>() : stdx::none};
                ASSERT(en, "Atomic builtin order/op argument must fold to a const_enum");
                return static_cast<u8>(en->value);
            };

            if (fn_token == syntax::token_type_t::BUILTIN_FENCE) {
                builder_.emit_builtin_call(name, {}, ret_type, stdx::none, eval_order(0));
                return value{void_val{}, ret_type};
            }

            const bool  has_t_arg{fn_token != syntax::token_type_t::BUILTIN_ATOMIC_STORE};
            const usize ptr_idx{has_t_arg ? 1UZ : 0UZ};

            std::vector<value> args;
            args.emplace_back(emit_expression(*call.arguments[ptr_idx].as_opt<ast::expr_handle>()));

            switch (fn_token) {
            case syntax::token_type_t::BUILTIN_ATOMIC_LOAD: {
                const auto order{eval_order(2)};
                const auto res{
                    builder_.emit_builtin_call(name, std::move(args), ret_type, stdx::none, order)};
                return res ? value{*res, ret_type} : value{void_val{}, ret_type};
            }
            case syntax::token_type_t::BUILTIN_ATOMIC_STORE: {
                args.emplace_back(emit_expression(*call.arguments[1].as_opt<ast::expr_handle>()));
                const auto order{eval_order(2)};
                builder_.emit_builtin_call(name, std::move(args), ret_type, stdx::none, order);
                return value{void_val{}, ret_type};
            }
            case syntax::token_type_t::BUILTIN_ATOMIC_RMW: {
                const auto op{eval_order(2)};
                args.emplace_back(emit_expression(*call.arguments[3].as_opt<ast::expr_handle>()));
                const auto order{eval_order(4)};
                const auto res{
                    builder_.emit_builtin_call(name, std::move(args), ret_type, op, order)};
                return res ? value{*res, ret_type} : value{void_val{}, ret_type};
            }
            case syntax::token_type_t::BUILTIN_CMPXCHG_WEAK:
            case syntax::token_type_t::BUILTIN_CMPXCHG_STRONG: {
                args.emplace_back(emit_expression(*call.arguments[2].as_opt<ast::expr_handle>()));
                args.emplace_back(emit_expression(*call.arguments[3].as_opt<ast::expr_handle>()));
                const auto succ{eval_order(4)};
                const auto fail{eval_order(5)};
                args.emplace_back(
                    emit_expression_id_raw(*call.arguments[6].as_opt<ast::expr_handle>()));
                const auto res{builder_.emit_builtin_call(
                    name, std::move(args), ret_type, stdx::none, succ, fail)};
                return res ? value{*res, ret_type} : value{void_val{}, ret_type};
            }
            default: UNREACHABLE("Unhandled atomic builtin");
            }
        }
        default: {
            if (const auto cv{const_eval_.try_eval(id)}) { return cv->to_gir_value(); }
            break;
        }
        }
    }

    stdx::option<std::string> callee_name;
    stdx::option<value>       indirect_callee;
    stdx::option<value>       dyn_self_data; // `w.data` for a `&dyn I` method call
    bool                      is_closure_ident_call{false};
    bool                      is_fn_ctx_self_call{false};
    const auto                dot_call{active_ast().get_as_opt<ast::dot_expr>(call.function)};

    // @fnCtx()(args): the inner call is never emitted, its callee names the enclosing function
    const auto fn_ctx_call{active_ast().get_as_opt<ast::call_expr>(call.function)};
    const bool is_fn_ctx_call{fn_ctx_call && fn_ctx_call->function->get_token_type() ==
                                                 syntax::token_type_t::BUILTIN_FN_CTX};

    // The resolver pinned this call's monomorphization (keyed on types *and* constexpr values).
    bool resolved_generic_target{false};
    if (const auto generic_target{active_mod().get_generic_call_target_opt(id)}) {
        callee_name.emplace(*generic_target);
        resolved_generic_target = true;
    } else if (is_fn_ctx_call) {
        ASSERT(!open_fn_names_.empty(), "@fnCtx() must be inside an open function");
        callee_name.emplace(open_fn_names_.back());
        is_fn_ctx_self_call = open_fn_is_closure_.back();
    } else if (const auto ident{active_ast().get_as_opt<ast::identifier_expr>(call.function)}) {
        if (const auto binding{lookup_binding(ident->name)}) {
            const bool is_fn_ptr_binding{
                binding->type.get_kind() == sema::type_kind::POINTER &&
                binding->type.get_data().as_opt<sema::types::pointer>() &&
                binding->type.get_data().as_opt<sema::types::pointer>()->underlying.get_kind() ==
                    sema::type_kind::FUNCTION};
            // Function-type parameters declared by value need to be invoked through indirection.
            const bool is_fn_val_binding{binding->type.get_kind() == sema::type_kind::FUNCTION};
            if (binding->const_val && binding->const_val->template is<std::string>()) {
                callee_name.emplace(binding->const_val->template as<std::string>());
            } else if (is_fn_ptr_binding || is_fn_val_binding) {
                if (binding->is_alloca) {
                    const auto loaded{
                        builder_.emit_load(value{binding->id, binding->type}, binding->type)};
                    indirect_callee.emplace(value{loaded, binding->type});
                } else {
                    indirect_callee.emplace(value{binding->id, binding->type});
                }
            } else if (binding->type.get_kind() == sema::type_kind::CLOSURE) {
                is_closure_ident_call = true;
                callee_name.emplace(fmt::format("closure{}", binding->type.get_symbol_table_idx()));
            } else {
                callee_name.emplace(std::string{ident->name});
            }
        } else if (const auto cv{const_eval_.try_eval(call.function)};
                   cv && cv->template is<std::string>() && cv->get_type() &&
                   cv->get_type()->get_kind() == sema::type_kind::FUNCTION) {
            callee_name.emplace(cv->template as<std::string>());
        } else {
            callee_name.emplace(std::string{ident->name});
        }
    } else if (dot_call) {
        const auto member_ident{active_ast().get_as<ast::identifier_expr>(dot_call->member)};
        const auto fn_ty{active_mod().get_sema_type_opt(call.function)};
        stdx::option<const sema::types::function&> fn_d;
        if (fn_ty) {
            if (const auto ptr_d{fn_ty->get_data().as_opt<sema::types::pointer>()}) {
                fn_d = ptr_d->underlying.get_data().as_opt<sema::types::function>();
            } else {
                fn_d = fn_ty->get_data().as_opt<sema::types::function>();
            }
        }

        // `w.method(...)` on a `&dyn I`: load the method from `w`'s vtable and call it indirectly
        const auto                      recv_ty{active_mod().get_sema_type_opt(dot_call->object)};
        stdx::option<const sema::type&> recv_dyn;
        if (recv_ty) {
            if (const auto p{recv_ty->get_data().as_opt<sema::types::pointer>()}) {
                recv_dyn.emplace(p->underlying);
            } else if (const auto r{recv_ty->get_data().as_opt<sema::types::reference>()}) {
                recv_dyn.emplace(r->underlying);
            }
        }

        if (recv_dyn && recv_dyn->get_kind() == sema::type_kind::DYN) {
            const auto& dyn{recv_dyn->get_data().as<sema::types::dyn_t>()};
            const auto& iface{dyn.interface.get_data().as<sema::types::interface_t>()};
            u64         slot_idx{0};
            for (usize i{0}; i < iface.method_names.size(); ++i) {
                if (iface.method_names[i] == member_ident.name) {
                    slot_idx = i;
                    break;
                }
            }
            auto& opaque_ty{ctx_.get_builtin_resolved_type(sema::type_kind::OPAQUE)};
            auto& ptr_ty{ctx_.get_pointer(sema::types::mut::CONSTANT, opaque_ty)};
            auto& ptr_to_ptr_ty{ctx_.get_pointer(sema::types::mut::CONSTANT, ptr_ty)};
            auto& usize_ty{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
            auto& dyn_mut{const_cast<sema::type&>(*recv_dyn)};

            // Spill `w` to get its address, then read the `{ data, vtable }` fields.
            const auto fat{value{emit_expression_id_raw(*dot_call->object).data, dyn_mut}};
            const auto fat_slot{builder_.emit_alloca(dyn_mut)};
            builder_.emit_store(value{fat_slot, dyn_mut}, fat);
            const auto d_ptr{builder_.emit_get_element_ptr(
                value{fat_slot, dyn_mut}, {value{u64{0}, usize_ty}}, ptr_ty)};
            const auto v_ptr{builder_.emit_get_element_ptr(
                value{fat_slot, dyn_mut}, {value{u64{1}, usize_ty}}, ptr_ty)};

            auto& self_ty{fn_d && !fn_d->params.empty() ? const_cast<sema::type&>(*fn_d->params[0])
                                                        : ptr_ty};
            dyn_self_data.emplace(
                value{builder_.emit_load(value{d_ptr, ptr_ty}, self_ty), self_ty});

            const auto vtable{builder_.emit_load(value{v_ptr, ptr_ty}, ptr_ty)};
            const auto slot_ptr{builder_.emit_get_element_ptr(
                value{vtable, ptr_to_ptr_ty}, {value{slot_idx, usize_ty}}, ptr_ty)};
            auto&      callee_ty{fn_ty ? const_cast<sema::type&>(*fn_ty) : ptr_ty};
            indirect_callee.emplace(
                value{builder_.emit_load(value{slot_ptr, ptr_ty}, callee_ty), callee_ty});
        } else if (fn_d && !fn_d->has_self) {
            // Dispatch `Type.method(...)` directly to the method's scoped symbol
            stdx::option<const sema::type&> rt{recv_ty};
            const bool recv_is_type{rt && (rt->get_kind() == sema::type_kind::TYPE ||
                                           rt->get_data().is<sema::types::meta_type>())};
            if (rt) {
                if (const auto m{rt->get_data().as_opt<sema::types::meta_type>()}) {
                    rt.emplace(m->instance);
                }
            }
            const bool static_on_type{recv_is_type && rt != nullptr && rt->has_symbol_table_idx() &&
                                      (rt->get_kind() == sema::type_kind::ENUM ||
                                       rt->get_kind() == sema::type_kind::STRUCT ||
                                       rt->get_kind() == sema::type_kind::UNION)};
            if (static_on_type) {
                // The static method lives in the receiver type's own member scope.
                callee_name.emplace(
                    symbol_scoping_.name_for(rt->get_symbol_table_idx(), member_ident.name));
            } else if (recv_is_type) {
                if (const auto owner{active_mod().get_resolved_symbol_owner_opt(call.function)}) {
                    callee_name.emplace(symbol_scoping_.name_for(*owner, member_ident.name));
                } else {
                    callee_name.emplace(std::string{member_ident.name});
                }
            } else {
                const auto callee_val{emit_expression(call.function)};
                indirect_callee.emplace(callee_val);
            }
        } else if (emitting_impl_default_scope_) {
            // Inside an inherited default body: `self.method(...)` targets this impl's method.
            callee_name.emplace(
                symbol_scoping_.name_for(*emitting_impl_default_scope_, member_ident.name));
        } else if (emitting_impl_body_scope_ && fn_d && fn_d->has_self && [&] {
                       // Inside a method written in an `impl` block, a call on the method's own
                       // receiver resolves to that impl's own scoped symbol so cross-module method
                       // names stay resolvable
                       if (user_type_stack_.empty() || !recv_ty ||
                           !active_ast().get_as_opt<ast::identifier_expr>(dot_call->object)) {
                           return false;
                       }
                       stdx::option<const sema::type&> r{recv_ty};
                       if (const auto p{r->get_data().as_opt<sema::types::pointer>()}) {
                           r.emplace(p->underlying);
                       } else if (const auto rf{r->get_data().as_opt<sema::types::reference>()}) {
                           r.emplace(rf->underlying);
                       }
                       return r == user_type_stack_.back();
                   }()) {
            callee_name.emplace(
                symbol_scoping_.name_for(*emitting_impl_body_scope_, member_ident.name));
        } else {
            callee_name.emplace(std::string{member_ident.name});
        }
    } else if (const auto imp_call{
                   active_ast().get_as_opt<ast::implicit_access_expr>(call.function)}) {
        // e.g. `const a: T = .init();` -- a no-self member called via implicit access.
        const auto member_ident{active_ast().get_as<ast::identifier_expr>(imp_call->member)};
        callee_name.emplace(std::string{member_ident.name});
    } else if (const auto fn_expr{active_ast().get_as_opt<ast::function_expr>(call.function)}) {
        callee_name.emplace(emit_anonymous_function(*call.function, *fn_expr));
    } else if (const auto mod_call{
                   active_ast().get_as_opt<ast::module_access_expr>(call.function)}) {
        const auto& inner_ident{active_ast().get_as<ast::identifier_expr>(mod_call->inner)};
        callee_name.emplace(std::string{inner_ident.name});
    } else {
        const auto callee_val{emit_expression(call.function)};
        if (callee_val.type && (callee_val.type->get_kind() == sema::type_kind::FUNCTION ||
                                callee_val.type->get_kind() == sema::type_kind::POINTER)) {
            indirect_callee.emplace(callee_val);
        } else {
            callee_name.emplace(fmt::format("anonymous_fn{}", anon_fn_counter_++));
        }
    }

    // A member call whose receiver is a `fn(...): type` constructor instantiation targets that
    // instantiation's monomorphized member
    if (callee_name && !indirect_callee && !resolved_generic_target) {
        stdx::option<sema::type&>      recv_type;
        stdx::option<std::string_view> member_nm;
        if (dot_call) {
            recv_type = active_mod().get_sema_type_opt(dot_call->object);
            member_nm = active_ast().get_as<ast::identifier_expr>(dot_call->member).name;
        } else if (const auto imp{
                       active_ast().get_as_opt<ast::implicit_access_expr>(call.function)}) {
            if (!user_type_stack_.empty()) { recv_type.emplace(*user_type_stack_.back()); }
            member_nm = active_ast().get_as<ast::identifier_expr>(imp->member).name;
        }
        if (recv_type && member_nm) {
            auto* t{recv_type.get()};
            if (const auto m{t->get_data().as_opt<sema::types::meta_type>()}) { t = &m->instance; }
            if (const auto p{t->get_data().as_opt<sema::types::pointer>()}) { t = &p->underlying; }
            if (const auto r{t->get_data().as_opt<sema::types::reference>()}) {
                t = &r->underlying;
            }
            if (const auto prefix{ctx_.generic_functions.get_type_ctor_member_prefix(*t)}) {
                callee_name.emplace(fmt::format("{}.{}", *prefix, *member_nm));
                resolved_generic_target = true;
            }
        }
    }

    // Disambiguate a direct call whose target the resolver bound to a specific module/type
    if (callee_name && !indirect_callee && !resolved_generic_target) {
        if (const auto owner{active_mod().get_resolved_symbol_owner_opt(call.function)}) {
            callee_name.emplace(symbol_scoping_.name_for(*owner, *callee_name));
        }
    }

    const auto fn_type_opt{active_mod().get_sema_type_opt(call.function)};
    stdx::option<const sema::types::function&> fn_data;
    if (fn_type_opt) {
        if (const auto ptr_data{fn_type_opt->get_data().as_opt<sema::types::pointer>()}) {
            fn_data = ptr_data->underlying.get_data().as_opt<sema::types::function>();
        } else if (const auto cl_data{fn_type_opt->get_data().as_opt<sema::types::closure_t>()}) {
            fn_data = cl_data->impl_signature.get_data().as_opt<sema::types::function>();
        } else {
            fn_data = fn_type_opt->get_data().as_opt<sema::types::function>();
        }
    }

    // Inside an inherited default body, `self.method(...)` was sema-resolved against the abstract
    // interface (`self: &interface`).
    if (emitting_impl_default_scope_ && dot_call) {
        const auto mn{active_ast().get_as<ast::identifier_expr>(dot_call->member).name};
        for (const auto* r : ctx_.impls.records()) {
            if (r->body_scope_idx != *emitting_impl_default_scope_) { continue; }
            if (const auto m{r->find_method(mn)}; m && m->fn_type) {
                // Retarget its signature to this impl's concrete method so the
                // implicit-self argument is typed for the real target.
                fn_data = m->fn_type->get_data().as_opt<sema::types::function>();
            }
            break;
        }
    }

    // A recursive closure call needs the self-inclusive impl signature, not the public one
    if (is_fn_ctx_self_call && fn_type_opt) {
        const auto idx{fn_type_opt->get_symbol_table_idx()};
        auto&      cl_type{*ctx_.pool[{sema::type_kind::CLOSURE, sema::types::mut::CONSTANT, idx}]};
        if (cl_type.is_resolved()) {
            fn_data = cl_type.get_data()
                          .as_opt<sema::types::closure_t>()
                          ->impl_signature.get_data()
                          .as_opt<sema::types::function>();
        }
    }

    bool is_obj_instance{false};
    if (dot_call) {
        bool       is_type{false};
        const auto target_obj{dot_call->object};
        if (const auto ident{active_ast().get_as_opt<ast::identifier_expr>(target_obj)}) {
            if (const auto root_table{active_mod().root_table_idx}) {
                if (const auto sym{ctx_.registry.get_from_opt(*root_table, ident->name)}) {
                    if (sym->has_kind() && sym->get_kind() == sema::symbol_kind::TYPE) {
                        is_type = true;
                    }
                }
            }
        } else if (const auto mac{active_ast().get_as_opt<ast::module_access_expr>(target_obj)}) {
            if (const auto mod_type{active_mod().get_sema_type_opt(mac->outer)}) {
                if (const auto m_data{mod_type->get_data().as_opt<sema::types::module>()}) {
                    const auto& inner_mod{m_data->imported};
                    if (inner_mod.root_table_idx) {
                        const auto& inner_ident{
                            active_ast().get_as<ast::identifier_expr>(mac->inner)};
                        if (const auto sym{ctx_.registry.get_from_opt(*inner_mod.root_table_idx,
                                                                      inner_ident.name)}) {
                            if (sym->has_kind() && sym->get_kind() == sema::symbol_kind::TYPE) {
                                is_type = true;
                            }
                        }
                    }
                }
            }
        }
        if (!is_type) { is_obj_instance = true; }
    }

    std::vector<value> args;
    const bool         has_implicit_self{is_closure_ident_call || is_fn_ctx_self_call ||
                                 dyn_self_data.has_value() ||
                                 (fn_data && fn_data->has_self && is_obj_instance)};
    args.reserve(call.arguments.size() + (has_implicit_self ? 1 : 0));

    if (dyn_self_data) {
        args.emplace_back(*dyn_self_data);
    } else if (is_closure_ident_call) {
        // The callee's binding is the environment; spill it to an alloca to get its address.
        ASSERT(!fn_data->params.empty(), "Closure implementation signature must have self");
        const auto self_ptr{emit_lvalue(call.function)};
        args.emplace_back(value{self_ptr.data, const_cast<sema::type&>(*fn_data->params[0])});
    } else if (is_fn_ctx_self_call) {
        // Recursing into a closure via @fnCtx(): reuse this body's own self binding
        const auto binding{lookup_binding("self")};
        ASSERT(binding, "@fnCtx() inside a closure must have a self binding");
        ASSERT(fn_data && !fn_data->params.empty(),
               "Closure implementation signature must have self");
        args.emplace_back(value{binding->id, const_cast<sema::type&>(*fn_data->params[0])});
    } else if (has_implicit_self && !fn_data->params.empty()) {
        auto&      self_param_type{const_cast<sema::type&>(*fn_data->params[0])};
        const auto obj_expr_h{dot_call->object};
        const auto obj_type{active_mod().get_sema_type_opt(obj_expr_h)};
        const auto self_kind{self_param_type.get_kind()};

        // The receiver of a `&self`/`^self` method needs the same adjustment a `&`/`^`
        // argument gets
        if (self_kind == sema::type_kind::POINTER || self_kind == sema::type_kind::REFERENCE) {
            if (obj_type && (obj_type->get_kind() == sema::type_kind::POINTER ||
                             obj_type->get_kind() == sema::type_kind::REFERENCE)) {
                // A `^T`/`&T` receiver already yields an address, so pass its raw
                // value retyped to the self-param mode
                args.emplace_back(value{emit_expression_id_raw(*obj_expr_h).data, self_param_type});
            } else {
                // Anything else is an lvalue to take the address of.
                const auto addr{builder_.emit_address_of(emit_lvalue(obj_expr_h), self_param_type)};
                args.emplace_back(value{addr, self_param_type});
            }
        } else {
            args.emplace_back(emit_expression(obj_expr_h));
        }
    }

    // `constexpr` parameters are erased from a monomorph's signature, so skip their arguments.
    stdx::option<const ast::function_expr&> generic_target_fn;
    if (resolved_generic_target && fn_type_opt) {
        if (const auto gi{ctx_.generic_functions.get_opt(*fn_type_opt)}) {
            generic_target_fn.emplace(*gi->fn_expr);
        }
    }

    const usize param_offset{has_implicit_self ? 1UZ : 0UZ};
    for (usize i{0}; const auto& arg : call.arguments) {
        const usize param_idx{i + param_offset};
        if (generic_target_fn && i < generic_target_fn->parameters.size() &&
            generic_target_fn->parameters[i].is_constexpr) {
            ++i;
            continue;
        }
        if (const auto expr_h{arg.as_opt<ast::expr_handle>()}) {
            bool is_type_arg{false};
            // A parameter declared `: type` accepts only a type value
            if (generic_target_fn && i < generic_target_fn->parameters.size()) {
                const auto& gp_type{generic_target_fn->parameters[i].explicit_type};
                if (gp_type.is_valid() &&
                    gp_type.get_token_type() == syntax::token_type_t::TYPE_TYPE) {
                    is_type_arg = true;
                }
            }
            if (const auto t_opt{active_mod().get_sema_type_opt(*expr_h)}) {
                if (t_opt->get_kind() == sema::type_kind::TYPE) { is_type_arg = true; }
            } else if (const auto ident{active_ast().get_as_opt<ast::identifier_expr>(*expr_h)}) {
                if (active_mod().root_table_idx) {
                    if (const auto sym{ctx_.registry.get_from_opt(*active_mod().root_table_idx,
                                                                  ident->name)}) {
                        if (sym->has_kind() && sym->get_kind() == sema::symbol_kind::TYPE) {
                            is_type_arg = true;
                        }
                    }
                }
                if (!is_type_arg && ctx_.prelude_index) {
                    if (const auto sym{
                            ctx_.registry.get_from_opt(*ctx_.prelude_index, ident->name)}) {
                        if (sym->has_kind() && sym->get_kind() == sema::symbol_kind::TYPE) {
                            is_type_arg = true;
                        }
                    }
                }
            }
            if (is_type_arg) {
                auto& type_type{ctx_.get_builtin_resolved_type(sema::type_kind::TYPE)};
                args.emplace_back(value{undefined_val{}, type_type});
            } else if (fn_data && param_idx < fn_data->params.size()) {
                args.emplace_back(emit_coerced_expr(*expr_h, *fn_data->params[param_idx]));
            } else {
                args.emplace_back(emit_expression(*expr_h));
            }
        } else if (const auto type_id{arg.as_opt<ast::explicit_type_id>()}) {
            auto& type_type{ctx_.get_builtin_resolved_type(sema::type_kind::TYPE)};
            args.emplace_back(value{undefined_val{}, type_type});
        }
        i++;
    }

    if (indirect_callee) {
        if (const auto dest{
                builder_.emit_indirect_call(*indirect_callee, std::move(args), ret_type)}) {
            return value{*dest, ret_type};
        }
        return value{void_val{}, ret_type};
    }

    // Fallback match by arg types only, for calls the resolver did not pin above.
    for (const auto& req : ast_module_.generic_instantiations) {
        if (resolved_generic_target) { break; }
        bool fn_match{false};
        if (fn_type_opt && *req.generic_fn_type == *fn_type_opt) {
            fn_match = true;
        } else if (const auto info{ctx_.generic_functions.get_opt(*req.generic_fn_type)};
                   info && info->name && *info->name == *callee_name) {
            fn_match = true;
        }

        if (fn_match) {
            // req.arg_types excludes the implicit self already prepended to args above.
            const auto explicit_args{
                gsl::span{args}.subspan(static_cast<usize>(has_implicit_self))};
            VERIFY(req.arg_types.size() == explicit_args.size(),
                   "Generic arg types do not match arity");
            bool args_match{true};
            for (const auto& [arg, arg_type] : std::views::zip(explicit_args, req.arg_types)) {
                if (arg.type && *arg_type != *arg.type &&
                    !sema::is_assignable(*arg.type, *arg_type)) {
                    args_match = false;
                    break;
                }
            }
            if (args_match) {
                callee_name.emplace(req.mangled_name);
                break;
            }
        }
    }

    if (const auto dest{builder_.emit_call(*callee_name, std::move(args), ret_type)}) {
        return value{*dest, ret_type};
    }
    return value{void_val{}, ret_type};
}

auto emitter::emit_if(ast::node_id id, const ast::if_expr& if_expr) -> value {
    PROFILE_FUNCTION();
    auto sema_type{active_mod().get_sema_type_opt(id)};
    if (if_expr.alternate &&
        if_expr.consequence->get_kind() == ast::node_kind::EXPRESSION_STATEMENT) {
        const auto& expr_st{active_ast().get_as<ast::expr_stmt>(*if_expr.consequence)};
        if (const auto expr_type = active_mod().get_sema_type_opt(expr_st.expression)) {
            if (expr_type->get_kind() != sema::type_kind::VOID_) { sema_type = expr_type; }
        }
    }
    const bool yields_value{if_expr.alternate && sema_type && is_value_type(sema_type->get_kind())};

    const auto emit_single_arm{[&](ast::stmt_handle arm) -> value {
        return yields_value ? emit_stmt_as_value(arm)
                            : (emit_stmt(arm), value{void_val{}, sema_type});
    }};

    // The type resolver already folded this `if constexpr`
    if (const auto it{active_mod().if_constexpr_results.find(id.get_index())};
        it != active_mod().if_constexpr_results.end()) {
        if (it->second == mod::if_branch::CONSEQUENCE) {
            return emit_single_arm(if_expr.consequence);
        }
        if (if_expr.alternate) { return emit_single_arm(*if_expr.alternate); }
        return value{void_val{}, sema_type};
    }

    // Constexpr condition evaluation fallback
    if (if_expr.constexpr_condition) {
        const auto cond_cv{const_eval_.try_eval(if_expr.condition)};
        if (!cond_cv) {
            ctx_.diags.emplace_back("Constexpr if condition could not be evaluated at compile time",
                                    sema::error::CONSTEXPR_EVALUATION_FAILED,
                                    active_ast().location_of(if_expr.condition));
            return value{undefined_val{}, sema_type};
        }

        const auto eval{cond_cv->as_opt<bool>()};
        if (!eval) {
            ctx_.diags.emplace_back("Constexpr if condition must evaluate to a boolean",
                                    sema::error::TYPE_MISMATCH,
                                    active_ast().location_of(if_expr.condition));
            return value{undefined_val{}, sema_type};
        }

        if (*eval) {
            return yields_value ? emit_stmt_as_value(if_expr.consequence)
                                : (emit_stmt(if_expr.consequence), value{void_val{}, sema_type});
        }
        if (if_expr.alternate) {
            return yields_value ? emit_stmt_as_value(*if_expr.alternate)
                                : (emit_stmt(*if_expr.alternate), value{void_val{}, sema_type});
        }
        return value{void_val{}, sema_type};
    }

    auto fn_opt{builder_.get_function()};
    ASSERT(fn_opt, "If expression must be inside an active function");
    auto& fn{*fn_opt};

    stdx::option<local_id> res_slot;
    if (yields_value) { res_slot.emplace(builder_.emit_alloca(*sema_type)); }
    const auto cond_val{coerce_condition(emit_expression(if_expr.condition))};

    auto&                  consequence_seg{fn.add_segment()};
    stdx::option<segment&> alternate_seg_ptr;
    if (if_expr.alternate) { alternate_seg_ptr.emplace(fn.add_segment()); }
    auto& merge_seg{fn.add_segment()};

    const auto false_target{alternate_seg_ptr ? alternate_seg_ptr->get_id() : merge_seg.get_id()};
    builder_.emit_cond_goto(cond_val, consequence_seg.get_id(), false_target);

    // Consequence branch
    builder_.set_segment(consequence_seg);
    if (yields_value) {
        builder_.emit_store(
            *res_slot, retype_if_undefined(emit_stmt_as_value(if_expr.consequence), *sema_type));
    } else {
        emit_stmt(if_expr.consequence);
    }
    if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
        builder_.emit_goto(merge_seg.get_id());
    }

    // Alternate branch
    if (alternate_seg_ptr) {
        builder_.set_segment(*alternate_seg_ptr);
        if (yields_value) {
            builder_.emit_store(
                *res_slot, retype_if_undefined(emit_stmt_as_value(*if_expr.alternate), *sema_type));
        } else {
            emit_stmt(*if_expr.alternate);
        }
        if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
            builder_.emit_goto(merge_seg.get_id());
        }
    }

    // Merge segment
    builder_.set_segment(merge_seg);
    if (yields_value) {
        const auto loaded{builder_.emit_load(*res_slot, *sema_type)};
        return value{loaded, sema_type};
    }
    return value{void_val{}, sema_type};
}

auto emitter::emit_while(ast::node_id                   id,
                         const ast::while_loop_expr&    while_loop,
                         stdx::option<std::string_view> label,
                         stdx::option<local_id>         res_slot,
                         stdx::option<sema::type&>      result_type) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{result_type ? result_type : active_mod().get_sema_type_opt(id)};
    const bool yields_value{sema_type && sema_type->get_kind() != sema::type_kind::VOID_};

    auto fn_opt{builder_.get_function()};
    ASSERT(fn_opt, "While loop must be within an active function");
    auto& fn{*fn_opt};
    if (yields_value && !res_slot) { res_slot = builder_.emit_alloca(*sema_type); }

    auto&                  cond_seg{fn.add_segment()};
    auto&                  body_seg{fn.add_segment()};
    stdx::option<segment&> continuation_seg;
    if (while_loop.continuation) { continuation_seg.emplace(fn.add_segment()); }
    stdx::option<segment&> non_break_seg;
    if (while_loop.non_break) { non_break_seg.emplace(fn.add_segment()); }
    auto& exit_seg{fn.add_segment()};

    if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
        builder_.emit_goto(cond_seg.get_id());
    }

    const auto continue_target{continuation_seg ? continuation_seg->get_id() : cond_seg.get_id()};
    {
        const loop_context_guard g{loop_stack_,
                                   loop_context{
                                       .label           = label,
                                       .break_target    = exit_seg.get_id(),
                                       .continue_target = continue_target,
                                       .result_slot     = res_slot,
                                   }};

        // Cond segment
        builder_.set_segment(cond_seg);
        const auto cond_val{coerce_condition(emit_expression(while_loop.condition))};
        const auto false_target{non_break_seg ? non_break_seg->get_id() : exit_seg.get_id()};
        builder_.emit_cond_goto(cond_val, body_seg.get_id(), false_target);

        // Body segment
        builder_.set_segment(body_seg);
        emit_block(active_ast().get_as<ast::block_stmt>(while_loop.block));
        if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
            builder_.emit_goto(continue_target);
        }

        // Continuation segment
        if (continuation_seg) {
            builder_.set_segment(*continuation_seg);
            emit_expression(*while_loop.continuation);
            if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
                builder_.emit_goto(cond_seg.get_id());
            }
        }
    }

    // Non-break / else branch
    if (non_break_seg) {
        builder_.set_segment(*non_break_seg);
        if (yields_value) {
            if (res_slot) {
                builder_.emit_store(*res_slot, emit_stmt_as_value(*while_loop.non_break));
            }
        } else {
            emit_stmt(*while_loop.non_break);
        }
        if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
            builder_.emit_goto(exit_seg.get_id());
        }
    }

    // Exit segment
    builder_.set_segment(exit_seg);
    if (yields_value && res_slot) {
        const auto loaded{builder_.emit_load(*res_slot, *sema_type)};
        return value{loaded, sema_type};
    }
    return value{void_val{}, sema_type};
}

auto emitter::emit_do_while(ast::node_id                   id,
                            const ast::do_while_loop_expr& do_while,
                            stdx::option<std::string_view> label,
                            stdx::option<local_id>         res_slot,
                            stdx::option<sema::type&>      result_type) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{result_type ? result_type : active_mod().get_sema_type_opt(id)};
    const bool yields_value{sema_type && sema_type->get_kind() != sema::type_kind::VOID_};

    auto fn_opt{builder_.get_function()};
    ASSERT(fn_opt, "Do-while loop must be within an active function");
    auto& fn{*fn_opt};
    if (yields_value && !res_slot) { res_slot = builder_.emit_alloca(*sema_type); }

    auto& body_seg{fn.add_segment()};
    auto& cond_seg{fn.add_segment()};
    auto& exit_seg{fn.add_segment()};

    if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
        builder_.emit_goto(body_seg.get_id());
    }

    {
        const loop_context_guard g{loop_stack_,
                                   loop_context{
                                       .label           = label,
                                       .break_target    = exit_seg.get_id(),
                                       .continue_target = cond_seg.get_id(),
                                       .result_slot     = res_slot,
                                   }};

        // Body segment
        builder_.set_segment(body_seg);
        emit_block(active_ast().get_as<ast::block_stmt>(do_while.block));
        if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
            builder_.emit_goto(cond_seg.get_id());
        }

        // Cond segment
        builder_.set_segment(cond_seg);
        const auto cond_val{coerce_condition(emit_expression(do_while.condition))};
        builder_.emit_cond_goto(cond_val, body_seg.get_id(), exit_seg.get_id());
    }

    // Exit segment
    builder_.set_segment(exit_seg);
    if (yields_value && res_slot) {
        const auto loaded{builder_.emit_load(*res_slot, *sema_type)};
        return value{loaded, sema_type};
    }
    return value{void_val{}, sema_type};
}

auto emitter::emit_infinite_loop(ast::node_id                   id,
                                 const ast::infinite_loop_expr& loop,
                                 stdx::option<std::string_view> label,
                                 stdx::option<local_id>         res_slot,
                                 stdx::option<sema::type&>      result_type) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{result_type ? result_type : active_mod().get_sema_type_opt(id)};
    const bool yields_value{sema_type && sema_type->get_kind() != sema::type_kind::VOID_};

    auto fn_opt{builder_.get_function()};
    ASSERT(fn_opt, "Infinite loop must be within an active function");
    auto& fn{*fn_opt};

    if (yields_value && !res_slot) { res_slot = builder_.emit_alloca(*sema_type); }

    auto& body_seg{fn.add_segment()};
    auto& exit_seg{fn.add_segment()};

    if (const auto cur_seg = builder_.get_segment(); cur_seg && !cur_seg->has_terminator()) {
        builder_.emit_goto(body_seg.get_id());
    }

    {
        const loop_context_guard g{loop_stack_,
                                   loop_context{
                                       .label           = label,
                                       .break_target    = exit_seg.get_id(),
                                       .continue_target = body_seg.get_id(),
                                       .result_slot     = res_slot,
                                   }};

        // Body segment
        builder_.set_segment(body_seg);
        emit_block(active_ast().get_as<ast::block_stmt>(loop.block));
        if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
            builder_.emit_goto(body_seg.get_id());
        }
    }

    // Exit segment
    builder_.set_segment(exit_seg);
    if (yields_value && res_slot) {
        const auto loaded{builder_.emit_load(*res_slot, *sema_type)};
        return value{loaded, sema_type};
    }
    return value{void_val{}, sema_type};
}

auto emitter::emit_for(ast::node_id                   id,
                       const ast::for_loop_expr&      for_loop,
                       stdx::option<std::string_view> label,
                       stdx::option<local_id>         res_slot,
                       stdx::option<sema::type&>      result_type) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{result_type ? result_type : active_mod().get_sema_type_opt(id)};
    const bool yields_value{sema_type && sema_type->get_kind() != sema::type_kind::VOID_};

    auto fn_opt{builder_.get_function()};
    ASSERT(fn_opt, "For loop must be within an active function");
    auto& fn{*fn_opt};

    if (yields_value && !res_slot) { res_slot.emplace(builder_.emit_alloca(*sema_type)); }
    std::vector<iterable_info> iter_infos;
    iter_infos.reserve(for_loop.iterables.size());

    for (const auto& [iter_handle, capture] :
         std::views::zip(for_loop.iterables, for_loop.captures)) {
        stdx::option<std::string_view> cap_name;
        stdx::option<sema::type&>      cap_type;
        if (capture.payload.is<ast::identifier_expr>()) {
            cap_name.emplace(active_ast().get_as<ast::identifier_expr>(capture.payload).name);
            cap_type = active_mod().get_sema_type_opt(capture.payload);
        }
        const bool alias_capture{capture.modifier.is_ref() || capture.modifier.is_ptr()};

        const auto iter_id{*iter_handle};
        if (const auto range{active_ast().get_as_opt<ast::range_expr>(iter_id)}) {
            const bool inclusive{iter_id.get_token_type() == syntax::token_type_t::DOT_DOT_EQ};
            auto&      usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};

            // A missing lower bound counts from `0`; a missing upper bound (`for (arr, lo..)`)
            // leans on a sibling iterable to stop the loop.
            const auto start_val{range->lhs ? emit_expression(*range->lhs)
                                            : value{u64{0}, usize_type}};
            const bool open_upper{!range->rhs};
            const auto end_val{open_upper ? value{u64{0}, usize_type}
                                          : emit_expression(*range->rhs)};
            auto*      elem_type{start_val.type ? &*start_val.type : &ctx_.get_int(32, true)};

            const auto slot{builder_.emit_alloca(*elem_type, cap_name.value_or(""))};
            builder_.emit_store(slot, start_val);

            iter_infos.emplace_back<iterable_info>({
                .is_range         = true,
                .is_inclusive     = inclusive,
                .range_open_upper = open_upper,
                .var_slot         = slot,
                .elem_type        = elem_type,
                .end_val          = end_val,
                .capture_name     = cap_name,
                .capture_type     = cap_type,
                .alias_capture    = alias_capture,
            });
        } else {
            const auto arr_val{emit_lvalue(iter_handle)};
            auto&      usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
            const auto idx_slot{builder_.emit_alloca(usize_type)};
            builder_.emit_store(idx_slot, value{static_cast<u64>(0), usize_type});

            stdx::option<sema::type&> elem_type{ctx_.get_int(32, true)};
            value                     end_val{static_cast<u64>(0), usize_type};

            if (arr_val.type) {
                // The container's own mutability governs element const correctness
                if (const auto arr_data{arr_val.type->get_data().as_opt<sema::types::array>()}) {
                    elem_type.emplace(
                        *ctx_.pool.with_const(arr_data->underlying, arr_val.type->is_constant()));
                    end_val = value{static_cast<u64>(arr_data->len), usize_type};
                } else if (const auto sl_data{
                               arr_val.type->get_data().as_opt<sema::types::slice>()}) {
                    elem_type.emplace(
                        *ctx_.pool.with_const(sl_data->underlying, arr_val.type->is_constant()));

                    // Slices carry a runtime length so load it once ahead of the loop
                    const auto len_slot{builder_.emit_get_element_ptr(
                        arr_val, {value{SLICE_LEN_FIELD_INDEX, usize_type}}, usize_type)};
                    const auto len_val{builder_.emit_load(value{len_slot, usize_type}, usize_type)};
                    end_val = value{len_val, usize_type};
                }
            }

            iter_infos.emplace_back<iterable_info>({
                .is_range      = false,
                .is_inclusive  = false,
                .var_slot      = idx_slot,
                .elem_type     = elem_type,
                .end_val       = end_val,
                .capture_name  = cap_name,
                .arr_val       = arr_val,
                .capture_type  = cap_type,
                .alias_capture = alias_capture,
            });
        }
    }

    auto&                  cond_seg{fn.add_segment()};
    auto&                  body_seg{fn.add_segment()};
    auto&                  step_seg{fn.add_segment()};
    stdx::option<segment&> non_break_seg;
    if (for_loop.non_break) { non_break_seg.emplace(fn.add_segment()); }
    auto& exit_seg{fn.add_segment()};

    if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
        builder_.emit_goto(cond_seg.get_id());
    }

    {
        const loop_context_guard ctx_g{loop_stack_,
                                       loop_context{
                                           .label           = label,
                                           .break_target    = exit_seg.get_id(),
                                           .continue_target = step_seg.get_id(),
                                           .result_slot     = res_slot,
                                       }};

        // Cond segment
        builder_.set_segment(cond_seg);
        auto& bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};

        value cond_val{true, bool_type};
        for (bool first{true}; const auto& info : iter_infos) {
            // An open-upper range (`for (arr, lo..)`) contributes no stop condition of its own.
            if (info.is_range && info.range_open_upper) { continue; }
            auto&      var_type{info.is_range ? *info.elem_type
                                              : ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
            const auto cur_val{builder_.emit_load(info.var_slot, var_type)};
            const auto cmp_kind{info.is_inclusive ? instruction_kind::LE : instruction_kind::LT};
            const auto cmp_res{
                builder_.emit_binary(cmp_kind, value{cur_val, var_type}, info.end_val, bool_type)};
            const value this_cond{cmp_res, bool_type};

            if (first) {
                cond_val = this_cond;
                first    = false;
            } else {
                const auto and_res{
                    builder_.emit_binary(instruction_kind::AND, cond_val, this_cond, bool_type)};
                cond_val = value{and_res, bool_type};
            }
        }

        const auto false_target{non_break_seg ? non_break_seg->get_id() : exit_seg.get_id()};
        builder_.emit_cond_goto(cond_val, body_seg.get_id(), false_target);

        // Body segment
        builder_.set_segment(body_seg);
        {
            const scope_guard g{scopes_};
            for (const auto& info : iter_infos) {
                if (!info.capture_name) { continue; }

                local_id elem_addr;
                if (info.is_range) {
                    elem_addr = info.var_slot;
                } else if (info.arr_val) {
                    auto&      usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
                    const auto idx_val{builder_.emit_load(info.var_slot, usize_type)};

                    // Force const so a plain capture is read-only
                    auto& read_only_elem_type{*ctx_.pool.with_const(*info.elem_type, true)};
                    if (info.arr_val->type &&
                        info.arr_val->type->get_data().as_opt<sema::types::slice>()) {
                        const auto& sl_data{
                            info.arr_val->type->get_data().as<sema::types::slice>()};
                        auto&      ptr_type{ctx_.get_pointer(info.arr_val->type->is_constant()
                                                            ? sema::types::mut::CONSTANT
                                                            : sema::types::mut::MUTABLE,
                                                        sl_data.underlying)};
                        const auto ptr_slot{builder_.emit_get_element_ptr(
                            *info.arr_val, {value{SLICE_PTR_FIELD_INDEX, usize_type}}, ptr_type)};
                        const auto ptr_val{builder_.emit_load(value{ptr_slot, ptr_type}, ptr_type)};
                        elem_addr = builder_.emit_get_element_ptr(
                            value{ptr_val, ptr_type},
                            std::vector<value>{value{idx_val, usize_type}},
                            read_only_elem_type);
                    } else {
                        elem_addr = builder_.emit_get_element_ptr(
                            *info.arr_val,
                            std::vector<value>{value{idx_val, usize_type}},
                            read_only_elem_type);
                    }
                } else {
                    continue;
                }

                // `|&v|`/`|&mut v|` and `|^v|`/`|^mut v|` alias the element's own address; a plain
                // `|v|` binds by value even when the element type is itself a `&`/`^`.
                if (info.alias_capture && info.capture_type) {
                    const auto capture_slot{builder_.emit_alloca(*info.capture_type)};
                    builder_.emit_store(capture_slot, value{elem_addr, *info.capture_type})
                        .is_initializer = true;
                    scopes_.back().bindings.emplace(*info.capture_name,
                                                    local_binding{
                                                        .id        = capture_slot,
                                                        .type      = *info.capture_type,
                                                        .is_alloca = true,
                                                        .const_val = stdx::none,
                                                    });
                } else if (info.is_range) {
                    // A range capture shares its address with the loop's own counter, which the
                    // step segment writes to; snapshot it as a read-only value instead of aliasing
                    const auto cur_val{builder_.emit_load(elem_addr, *info.elem_type)};
                    scopes_.back().bindings.emplace(
                        *info.capture_name,
                        local_binding{
                            .id        = local_id{0, local_kind::TEMPORARY},
                            .type      = *info.elem_type,
                            .is_alloca = false,
                            .const_val = value{cur_val, *info.elem_type},
                            .is_const  = true,
                        });
                } else {
                    // A plain capture is read-only regardless of the container's own mutability;
                    // `&mut`/`^mut` is required to write through it
                    scopes_.back().bindings.emplace(
                        *info.capture_name,
                        local_binding{
                            .id        = elem_addr,
                            .type      = *ctx_.pool.with_const(*info.elem_type, true),
                            .is_alloca = true,
                            .const_val = stdx::none,
                        });
                }
            }
            emit_block(active_ast().get_as<ast::block_stmt>(for_loop.block));
        }
        if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
            builder_.emit_goto(step_seg.get_id());
        }

        // Step segment
        builder_.set_segment(step_seg);
        for (const auto& info : iter_infos) {
            if (info.is_range) {
                const auto cur{builder_.emit_load(info.var_slot, *info.elem_type)};
                const auto next{builder_.emit_binary(instruction_kind::ADD,
                                                     value{cur, *info.elem_type},
                                                     value{static_cast<i64>(1), *info.elem_type},
                                                     *info.elem_type)};
                builder_.emit_store(info.var_slot, value{next, *info.elem_type});
            } else {
                auto&      usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
                const auto cur{builder_.emit_load(info.var_slot, usize_type)};
                const auto next{builder_.emit_binary(instruction_kind::ADD,
                                                     value{cur, usize_type},
                                                     value{static_cast<u64>(1), usize_type},
                                                     usize_type)};
                builder_.emit_store(info.var_slot, value{next, usize_type});
            }
        }

        if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
            builder_.emit_goto(cond_seg.get_id());
        }
    }

    // Non-break / else branch
    if (non_break_seg) {
        builder_.set_segment(*non_break_seg);
        if (yields_value) {
            if (res_slot) {
                builder_.emit_store(*res_slot, emit_stmt_as_value(*for_loop.non_break));
            }
        } else {
            emit_stmt(*for_loop.non_break);
        }
        if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
            builder_.emit_goto(exit_seg.get_id());
        }
    }

    // Exit segment
    builder_.set_segment(exit_seg);
    if (yields_value && res_slot) {
        return value{builder_.emit_load(*res_slot, *sema_type), sema_type};
    }
    return value{void_val{}, sema_type};
}

auto emitter::emit_label(ast::node_id id, const ast::label_expr& label) -> value {
    PROFILE_FUNCTION();
    const auto  sema_type{active_mod().get_sema_type_opt(id)};
    const bool  yields_value{sema_type && sema_type->get_kind() != sema::type_kind::VOID_};
    const auto& name_ident{active_ast().get_as<ast::identifier_expr>(label.name)};
    const auto  label_name{name_ident.name};

    stdx::option<local_id> res_slot;
    if (yields_value) { res_slot = builder_.emit_alloca(*sema_type); }

    const auto body_id{*label.body};
    return active_ast()[body_id].visit(
        [&](const auto&) -> value {
            auto fn_opt{builder_.get_function()};
            ASSERT(fn_opt, "Label expression must be within an active function");
            auto& fn{*fn_opt};

            auto& exit_seg{fn.add_segment()};
            {
                const loop_context_guard ctx_g{loop_stack_,
                                               loop_context{
                                                   .label           = label_name,
                                                   .break_target    = exit_seg.get_id(),
                                                   .continue_target = exit_seg.get_id(),
                                                   .result_slot     = res_slot,
                                               }};

                emit_expression_id(body_id);
                if (const auto cur_seg{builder_.get_segment()};
                    cur_seg && !cur_seg->has_terminator()) {
                    builder_.emit_goto(exit_seg.get_id());
                }
            }
            builder_.set_segment(exit_seg);

            if (yields_value && res_slot) {
                const auto loaded{builder_.emit_load(*res_slot, *sema_type)};
                return value{loaded, sema_type};
            }
            return value{void_val{}, sema_type};
        },
        [&](const ast::while_loop_expr& wl) -> value {
            return emit_while(body_id, wl, label_name, res_slot, sema_type);
        },
        [&](const ast::do_while_loop_expr& dw) -> value {
            return emit_do_while(body_id, dw, label_name, res_slot, sema_type);
        },
        [&](const ast::infinite_loop_expr& il) -> value {
            return emit_infinite_loop(body_id, il, label_name, res_slot, sema_type);
        },
        [&](const ast::for_loop_expr& fl) -> value {
            return emit_for(body_id, fl, label_name, res_slot, sema_type);
        },
        [&](const ast::block_stmt& block) -> value {
            auto fn_opt{builder_.get_function()};
            ASSERT(fn_opt, "Block label expression must be within an active function");
            auto& fn{*fn_opt};

            auto& exit_seg{fn.add_segment()};
            {
                const loop_context_guard g{loop_stack_,
                                           loop_context{
                                               .label           = label_name,
                                               .break_target    = exit_seg.get_id(),
                                               .continue_target = exit_seg.get_id(),
                                               .result_slot     = res_slot,
                                           }};

                emit_block(block);
                if (const auto cur_seg{builder_.get_segment()};
                    cur_seg && !cur_seg->has_terminator()) {
                    builder_.emit_goto(exit_seg.get_id());
                }
            }
            builder_.set_segment(exit_seg);

            if (yields_value && res_slot) {
                const auto loaded{builder_.emit_load(*res_slot, *sema_type)};
                return value{loaded, sema_type};
            }
            return value{void_val{}, sema_type};
        });
}

auto emitter::emit_logical_and(ast::node_id, const ast::binary_expr& binary) -> value {
    PROFILE_FUNCTION();
    auto&      bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
    const auto res_slot{builder_.emit_alloca(bool_type)};
    builder_.emit_store(res_slot, value{false, bool_type});
    const auto lhs_val{coerce_condition(emit_expression(binary.lhs))};

    auto fn_opt{builder_.get_function()};
    ASSERT(fn_opt, "Logical AND must be within an active function");
    auto& fn{*fn_opt};

    auto& rhs_seg{fn.add_segment()};
    auto& merge_seg{fn.add_segment()};

    builder_.emit_cond_goto(lhs_val, rhs_seg.get_id(), merge_seg.get_id());

    builder_.set_segment(rhs_seg);
    const auto rhs_val{coerce_condition(emit_expression(binary.rhs))};
    builder_.emit_store(res_slot, rhs_val);
    if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
        builder_.emit_goto(merge_seg.get_id());
    }

    builder_.set_segment(merge_seg);
    const auto loaded{builder_.emit_load(res_slot, bool_type)};
    return value{loaded, bool_type};
}

auto emitter::emit_logical_or(ast::node_id, const ast::binary_expr& binary) -> value {
    PROFILE_FUNCTION();
    auto&      bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
    const auto res_slot{builder_.emit_alloca(bool_type)};
    builder_.emit_store(res_slot, value{true, bool_type});

    const auto lhs_val{coerce_condition(emit_expression(binary.lhs))};

    auto fn_opt{builder_.get_function()};
    ASSERT(fn_opt, "Logical OR must be within an active function");
    auto& fn{*fn_opt};

    auto& rhs_seg{fn.add_segment()};
    auto& merge_seg{fn.add_segment()};
    builder_.emit_cond_goto(lhs_val, merge_seg.get_id(), rhs_seg.get_id());

    builder_.set_segment(rhs_seg);
    const auto rhs_val{coerce_condition(emit_expression(binary.rhs))};
    builder_.emit_store(res_slot, rhs_val);
    if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
        builder_.emit_goto(merge_seg.get_id());
    }

    builder_.set_segment(merge_seg);
    const auto loaded{builder_.emit_load(res_slot, bool_type)};
    return value{loaded, bool_type};
}

auto emitter::spill_to_temporary(value val, sema::type& type, bool is_const) -> value {
    const auto slot{builder_.emit_alloca(type, {}, is_const)};
    builder_.emit_store(value{slot, type}, val).is_initializer = true;
    return value{slot, type};
}

auto emitter::materialize_const(const const_value& cv) -> value {
    auto& usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};

    if (const auto arr{cv.as_opt<const_array>()}) {
        const auto type_opt{cv.get_type()};
        ASSERT(type_opt, "const_array must carry a resolved sema type");
        auto&      type{*type_opt};
        const auto arr_data{type.get_data().as_opt<sema::types::array>()};
        ASSERT(arr_data, "const_array must materialize into an array sema type");
        auto&      elem_type{arr_data->underlying};
        const auto slot{builder_.emit_alloca(type)};
        for (u64 i{0}; const auto& elem : arr->elements) {
            const auto elem_ptr{builder_.emit_get_element_ptr(
                value{slot, type}, {value{i++, usize_type}}, elem_type)};
            builder_.emit_store(value{elem_ptr, elem_type}, materialize_const(elem))
                .is_initializer = true;
        }
        const auto loaded{builder_.emit_load(value{slot, type}, type)};
        return value{loaded, type};
    }

    if (const auto st{cv.as_opt<const_struct>()}) {
        const auto type_opt{cv.get_type()};
        ASSERT(type_opt, "const_struct must carry a resolved sema type");
        auto&      type{*type_opt};
        const auto struct_data{type.get_data().as_opt<sema::types::struct_t>()};
        ASSERT(struct_data, "const_struct must materialize into a struct sema type");
        const auto  slot{builder_.emit_alloca(type)};
        const auto& table{ctx_.registry.get(type.get_symbol_table_idx())};
        for (const auto& [name, field_val] : st->fields) {
            const auto proxy{table.get_proxy_opt(name)};
            ASSERT(proxy, "const_struct field must exist in struct symbol table");
            const auto [sym, field_idx]{*proxy};
            auto&      field_type{struct_data->type_at(field_idx)};
            const auto field_ptr{builder_.emit_get_element_ptr(
                value{slot, type}, {value{static_cast<u64>(field_idx), usize_type}}, field_type)};
            builder_.emit_store(value{field_ptr, field_type}, materialize_const(field_val))
                .is_initializer = true;
        }
        const auto loaded{builder_.emit_load(value{slot, type}, type)};
        return value{loaded, type};
    }

    if (const auto un{cv.as_opt<const_union>()}) {
        const auto type_opt{cv.get_type()};
        ASSERT(type_opt, "const_union must carry a resolved sema type");
        auto&      type{*type_opt};
        const auto union_data{type.get_data().as_opt<sema::types::union_t>()};
        ASSERT(union_data, "const_union must materialize into a union sema type");
        const auto slot{builder_.emit_alloca(type)};
        ASSERT(!un->payload.empty(), "const_union must carry exactly one payload value");
        const auto& field_val{un->payload.front()};

        if (union_data->is_untagged) {
            builder_.emit_store(value{slot, type}, materialize_const(field_val)).is_initializer =
                true;
        } else {
            const auto& table{ctx_.registry.get(type.get_symbol_table_idx())};
            const auto  proxy{table.get_proxy_opt(un->active_field)};
            ASSERT(proxy, "const_union active field must exist in union symbol table");
            const auto [sym, field_idx]{*proxy};
            auto& field_type{union_data->type_at(field_idx)};

            auto&      i32_type{ctx_.get_int(32, true)};
            const auto tag_ptr{builder_.emit_get_element_ptr(
                value{slot, type}, {value{TAGGED_UNION_DISCRIMINANT_INDEX, usize_type}}, i32_type)};
            builder_
                .emit_store(value{tag_ptr, i32_type}, value{static_cast<i64>(field_idx), i32_type})
                .is_initializer = true;

            const auto payload_ptr{builder_.emit_get_element_ptr(
                value{slot, type}, {value{TAGGED_UNION_PAYLOAD_INDEX, usize_type}}, field_type)};
            builder_.emit_store(value{payload_ptr, field_type}, materialize_const(field_val))
                .is_initializer = true;
        }

        const auto loaded{builder_.emit_load(value{slot, type}, type)};
        return value{loaded, type};
    }

    return cv.to_gir_value();
}

auto emitter::lvalue_of_expr(ast::node_id id, sema::type& sema_type) -> value {
    if (const auto ref_data{sema_type.get_data().as_opt<sema::types::reference>()}) {
        auto& referent_type{const_cast<sema::type&>(ref_data->underlying)};
        return value{emit_expression_id_raw(id).data, referent_type};
    }
    // Spill using the type the emitted value actually carries
    auto  val{emit_expression_id_raw(id)};
    auto& spill_type{val.type ? const_cast<sema::type&>(*val.type) : sema_type};
    return spill_to_temporary(std::move(val), spill_type);
}

auto emitter::emit_panic_call(std::string_view message, ast::node_id site) -> void {
    const auto loc{active_ast().location_of(site)};
    auto&      u32_type{ctx_.get_int(32, false)};
    auto&      noreturn_type{ctx_.get_builtin_resolved_type(sema::type_kind::NORETURN)};

    std::vector<value> args;
    args.emplace_back(const_value::make_string(ctx_, std::string{message}).to_gir_value());
    args.emplace_back(const_value::make_string(ctx_, active_mod().path.string()).to_gir_value());
    args.emplace_back(value{static_cast<u64>(loc.line), u32_type});
    args.emplace_back(value{static_cast<u64>(loc.column), u32_type});

    builder_.emit_call("panic_handler", std::move(args), noreturn_type);
    builder_.emit_unreachable();
    request_builtin_runtime("panic_handler");
}

auto emitter::emit_null_pointer_check(value ptr, ast::node_id site) -> void {
    if (!runtime_safety_ || !ptr.type || ptr.type->get_kind() != sema::type_kind::POINTER) {
        return;
    }
    auto fn_opt{builder_.get_function()};
    if (!fn_opt) { return; }

    auto&      bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
    auto&      null_type{ctx_.get_builtin_resolved_type(sema::type_kind::NULLPTR)};
    const auto is_null{builder_.emit_binary(
        instruction_kind::EQ, std::move(ptr), value{nullptr_val{}, null_type}, bool_type)};

    auto& bad_seg{fn_opt->add_segment()};
    auto& ok_seg{fn_opt->add_segment()};
    builder_.emit_cond_goto(value{is_null, bool_type}, bad_seg.get_id(), ok_seg.get_id());

    builder_.set_segment(bad_seg);
    emit_panic_call("dereference of null pointer", site);
    builder_.set_segment(ok_seg);
}

auto emitter::enum_discriminants(const sema::types::enum_t& en) -> std::vector<i64> {
    std::vector<i64> discriminants;
    discriminants.reserve(en.ast_enumerations.size());

    // A variant's initializer node is only valid against the enum's defining module's AST arena,
    // which may differ from whichever module `const_eval_` is currently scoped to
    auto&      enclosing_mod{const_cast<mod::module&>(en.enclosing)};
    const_eval enclosing_eval{ctx_, enclosing_mod};
    enclosing_eval.set_symbol_scoping(symbol_scoping_);

    for (usize idx{0}; idx < en.ast_enumerations.size(); ++idx) {
        const auto& enumeration{en.ast_enumerations[idx]};
        i64         disc{static_cast<i64>(idx)};
        if (enumeration.value) {
            if (const auto ev{enclosing_eval.try_eval(*enumeration.value)}) {
                disc = static_cast<i64>(ev->as_int_opt().value_or(disc));
            }
        }
        discriminants.emplace_back(disc);
    }
    return discriminants;
}

auto emitter::emit_enum_cast_guard(ast::node_id     site,
                                   const value&     enum_val,
                                   const value&     src_val,
                                   ast::expr_handle src_expr) -> void {
    if (!runtime_safety_ || !enum_val.type || !src_val.type) { return; }

    // Only integer -> enum casts need guarding
    if (!sema::is_integer(src_val.type->get_kind())) { return; }
    const auto en{enum_val.type->get_data().as_opt<sema::types::enum_t>()};
    if (!en || en->non_exhaustive) { return; }

    const std::vector<i64> discriminants{enum_discriminants(*en)};
    if (discriminants.empty()) { return; }

    // A compile-time-known value that already lands on a variant needs no runtime check.
    if (const auto cv{const_eval_.try_eval(*src_expr)}) {
        if (const auto known{cv->as_int_opt()}) {
            if (std::ranges::contains(discriminants |
                                          std::views::transform([](i64 i) -> i128 { return i; }),
                                      *known)) {
                return;
            }
        }
    }

    auto fn_opt{builder_.get_function()};
    if (!fn_opt) { return; }

    auto& bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
    auto& underlying{en->underlying};

    // Compare against the discriminants in the enum's underlying integer type.
    const value raw_val{builder_.emit_cast(instruction_kind::BIT_CAST, enum_val, underlying),
                        underlying};

    // is_valid = (v == d0) || (v == d1) || ...
    stdx::option<value> is_valid;
    for (const auto disc : discriminants) {
        const value rhs{static_cast<u64>(disc), underlying};
        const auto  eq{builder_.emit_binary(instruction_kind::EQ, raw_val, rhs, bool_type)};
        const value eq_val{eq, bool_type};
        if (!is_valid) {
            is_valid.emplace(eq_val);
        } else {
            is_valid.emplace(
                builder_.emit_binary(instruction_kind::OR, *is_valid, eq_val, bool_type),
                bool_type);
        }
    }

    auto& ok_seg{fn_opt->add_segment()};
    auto& bad_seg{fn_opt->add_segment()};
    builder_.emit_cond_goto(*is_valid, ok_seg.get_id(), bad_seg.get_id());

    builder_.set_segment(bad_seg);
    emit_panic_call("invalid enum value", site);
    builder_.set_segment(ok_seg);
}

auto emitter::materialize_string_slice(std::string_view text, sema::type& slice_type) -> value {
    auto& usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
    auto& slice_data{slice_type.get_data().as<sema::types::slice>()};
    // Mutable, even though the slice itself may be `[]const u8`: this only types the scratch
    // field address used to initialize `slot`, not the loaded result handed back to the caller.
    auto&      elem_ptr_type{ctx_.get_pointer(sema::types::mut::MUTABLE, slice_data.underlying)};
    const auto slot{builder_.emit_alloca(slice_type)};

    // A slice value can't be written with a single `store`
    const auto ptr_field{builder_.emit_get_element_ptr(
        value{slot, slice_type}, {value{SLICE_PTR_FIELD_INDEX, usize_type}}, elem_ptr_type)};
    builder_.emit_store(value{ptr_field, elem_ptr_type}, value{std::string{text}, elem_ptr_type})
        .is_initializer = true;
    const auto len_field{builder_.emit_get_element_ptr(
        value{slot, slice_type}, {value{SLICE_LEN_FIELD_INDEX, usize_type}}, usize_type)};
    builder_
        .emit_store(value{len_field, usize_type}, value{static_cast<u64>(text.size()), usize_type})
        .is_initializer = true;

    return value{builder_.emit_load(value{slot, slice_type}, slice_type), slice_type};
}

auto emitter::emit_runtime_tag_name(ast::expr_handle operand_expr,
                                    sema::type&      operand_type,
                                    sema::type&      ret_type) -> stdx::option<value> {
    const auto en{operand_type.get_data().as_opt<sema::types::enum_t>()};
    const auto ut{operand_type.get_data().as_opt<sema::types::union_t>()};
    if ((!en) && (!ut || ut->is_untagged)) { return stdx::none; }

    auto fn_opt{builder_.get_function()};
    if (!fn_opt) { return stdx::none; }
    auto& fn{*fn_opt};

    auto&      bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
    auto&      usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
    auto&      merge_seg{fn.add_segment()};
    const auto res_slot{builder_.emit_alloca(ret_type)};

    const auto store_name{[&](std::string_view name) {
        builder_.emit_store(value{res_slot, ret_type}, materialize_string_slice(name, ret_type));
    }};

    // Tests one discriminant/tag value; if it matches, stores `name` and joins the merge
    // segment, mirroring `emit_match`'s per-arm dispatch shape.
    const auto emit_named_case{[&](value cond, std::string_view name) {
        auto& body_seg{fn.add_segment()};
        auto& next_seg{fn.add_segment()};
        builder_.emit_cond_goto(cond, body_seg.get_id(), next_seg.get_id());

        builder_.set_segment(body_seg);
        store_name(name);
        builder_.emit_goto(merge_seg.get_id());

        builder_.set_segment(next_seg);
    }};

    if (en) {
        const auto  operand_val{emit_expression(operand_expr)};
        auto&       underlying{en->underlying};
        const value raw_val{builder_.emit_cast(instruction_kind::BIT_CAST, operand_val, underlying),
                            underlying};
        const auto  discriminants{enum_discriminants(*en)};
        for (usize idx{0}; idx < en->ast_enumerations.size(); ++idx) {
            const auto& vname{
                en->enclosing.ast.get_as<ast::identifier_expr>(en->ast_enumerations[idx].name)
                    .name};
            const value rhs{static_cast<u64>(discriminants[idx]), underlying};
            const auto  eq{builder_.emit_binary(instruction_kind::EQ, raw_val, rhs, bool_type)};
            emit_named_case(value{eq, bool_type}, vname);
        }

        if (en->non_exhaustive) {
            // A non-exhaustive enum may legitimately hold a raw value with no listed variant.
            store_name("_");
            builder_.emit_goto(merge_seg.get_id());
        } else {
            // Every valid value of an exhaustive enum was just tested above.
            builder_.emit_unreachable();
        }
    } else {
        // Address of the scrutinee: reuse its storage when it is an lvalue, else spill the rvalue.
        const bool is_lvalue_shape{active_ast().get_as_opt<ast::identifier_expr>(operand_expr) ||
                                   active_ast().get_as_opt<ast::dot_expr>(operand_expr) ||
                                   active_ast().get_as_opt<ast::index_expr>(operand_expr) ||
                                   active_ast().get_as_opt<ast::dereference_expr>(operand_expr)};
        const auto operand_addr{is_lvalue_shape ? emit_lvalue(operand_expr)
                                                : spill_to_temporary(emit_expression(operand_expr),
                                                                     operand_type,
                                                                     operand_type.is_constant())};

        auto&       i32_type{ctx_.get_int(32, true)};
        const auto  tag_ptr{builder_.emit_get_element_ptr(
            operand_addr, {value{TAGGED_UNION_DISCRIMINANT_INDEX, usize_type}}, i32_type)};
        const value tag_val{builder_.emit_load(value{tag_ptr, i32_type}, i32_type), i32_type};

        for (usize idx{0}; idx < ut->ast_fields.size(); ++idx) {
            const auto& fname{
                ut->enclosing.ast.get_as<ast::identifier_expr>(ut->ast_fields[idx].name).name};
            const value rhs{static_cast<u64>(idx), i32_type};
            const auto  eq{builder_.emit_binary(instruction_kind::EQ, tag_val, rhs, bool_type)};
            emit_named_case(value{eq, bool_type}, fname);
        }

        // Every tag a well-formed tagged union can hold names one of its declared fields.
        builder_.emit_unreachable();
    }

    builder_.set_segment(merge_seg);
    return value{builder_.emit_load(value{res_slot, ret_type}, ret_type), ret_type};
}

auto emitter::emit_checked_binary(instruction_kind kind,
                                  value            lhs,
                                  value            rhs,
                                  sema::type&      result_type,
                                  ast::node_id,
                                  bool wrapping) -> local_id {
    // Only integer arithmetic can trap, and only signed +/-/* can overflow.
    const auto k{result_type.get_kind()};
    const bool checkable{!wrapping && runtime_safety_ && sema::is_integer(k) &&
                         (((kind == instruction_kind::ADD || kind == instruction_kind::SUB ||
                            kind == instruction_kind::MUL) &&
                           sema::is_signed_integer(result_type)) ||
                          kind == instruction_kind::DIV || kind == instruction_kind::MOD ||
                          kind == instruction_kind::SHL || kind == instruction_kind::SHR)};
    return builder_.emit_binary(kind, std::move(lhs), std::move(rhs), result_type, checkable);
}

auto emitter::emit_checked_unary(instruction_kind kind,
                                 value            operand,
                                 sema::type&      result_type,
                                 ast::node_id,
                                 bool wrapping) -> local_id {
    const bool checkable{!wrapping && runtime_safety_ && kind == instruction_kind::NEG &&
                         sema::is_signed_integer(result_type)};
    return builder_.emit_unary(kind, std::move(operand), result_type, checkable);
}

auto emitter::pointer_to_bool(value ptr, bool invert) -> value {
    auto&      bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
    auto&      null_type{ctx_.get_builtin_resolved_type(sema::type_kind::NULLPTR)};
    const auto op{invert ? instruction_kind::EQ : instruction_kind::NE};
    return value{
        builder_.emit_binary(op, std::move(ptr), value{nullptr_val{}, null_type}, bool_type),
        bool_type};
}

auto emitter::coerce_condition(value cond) -> value {
    if (cond.type && cond.type->get_kind() == sema::type_kind::POINTER) {
        return pointer_to_bool(std::move(cond), false);
    }
    return cond;
}

auto emitter::request_builtin_runtime(std::string_view name) -> void {
    if (std::ranges::find(pending_builtin_runtime_, name) == pending_builtin_runtime_.end()) {
        pending_builtin_runtime_.emplace_back(name);
    }
}

auto emitter::ensure_builtin_runtime(std::string_view name) -> void {
    if (gir_module_.has_function(name)) { return; }
    if (!ctx_.modules.has_builtin_module()) { return; }

    auto& builtin_mod{ctx_.modules.builtin_module()};
    for (const auto root_id : builtin_mod.ast) {
        const auto decl{builtin_mod.ast.get_as_opt<ast::decl_stmt>(root_id)};
        if (!decl || !decl->name.is_valid()) { continue; }
        const auto ident{builtin_mod.ast.get_as_opt<ast::identifier_expr>(decl->name)};
        if (!ident || ident->name != name) { continue; }

        const auto prev_module{std::exchange(active_module_, builtin_mod)};
        const_eval_.set_module(builtin_mod);
        emit_top_level_decl(root_id, *decl);
        active_module_ = prev_module;
        const_eval_.set_module(*prev_module);
        break;
    }
}

auto emitter::emit_lvalue(ast::node_id id) -> value {
    PROFILE_FUNCTION();
    ASSERT(id.is_valid(), "Valid node ID expected in emit_lvalue");

    return active_ast()[id].visit(
        [&](const auto&) -> value {
            const auto sema_type{active_mod().get_sema_type_opt(id)};
            ASSERT(sema_type, "LValue expression must have a resolved sema type");
            return lvalue_of_expr(id, *sema_type);
        },
        [&](const ast::identifier_expr& ident) -> value {
            const auto binding{lookup_binding(ident.name)};
            if (!binding) {
                // Not a local binding; may be a top-level const/constexpr global.
                if (const_eval_.try_eval(id)) {
                    const auto sema_type{active_mod().get_sema_type_opt(id)};
                    ASSERT(sema_type, "LValue identifier must have a resolved sema type");
                    return spill_to_temporary(emit_ident(id, ident), *sema_type, true);
                }
                // A module-level `var`: its lvalue is the global's address.
                if (const auto gref{try_global_ref(ident.name)}) { return *gref; }
                // A bare `var` sibling static member inside a member fn body.
                if (!user_type_stack_.empty()) {
                    if (const auto gref{
                            try_static_member_ref(*user_type_stack_.back(), ident.name)}) {
                        return *gref;
                    }
                }
            }
            ASSERT(binding, "LValue identifier must be bound in scope");
            // Struct/union field mutability is binding-based
            const auto is_struct_or_union{binding->type.get_kind() == sema::type_kind::STRUCT ||
                                          binding->type.get_kind() == sema::type_kind::UNION};
            auto&      qualified_type{is_struct_or_union
                                          ? *ctx_.pool.with_const(binding->type, binding->is_const)
                                          : binding->type};
            if (binding->is_alloca) { return value{binding->id, qualified_type}; }

            // A binding with no alloca of its own has no address: spill it into one
            const auto is_const_binding{binding->const_val || binding->is_const};
            const auto unspilled_value{
                binding->const_val.value_or(value{binding->id, qualified_type})};
            const auto spilled{
                spill_to_temporary(unspilled_value, qualified_type, is_const_binding)};
            auto& mut_binding{*lookup_binding<local_binding&>(ident.name)};
            mut_binding.id        = spilled.data.as<local_id>();
            mut_binding.is_alloca = true;
            mut_binding.const_val = stdx::none;
            return spilled;
        },
        [&](const ast::call_expr& call) -> value {
            const auto fn_token{call.function->get_token_type()};
            const auto is_transparent_cast{fn_token == syntax::token_type_t::BUILTIN_CONST_CAST ||
                                           fn_token == syntax::token_type_t::BUILTIN_VOLATILE_CAST};
            if (is_transparent_cast) {
                // Akin to a reinterpret cast in c++, same storage, just trust me...
                if (const auto op_expr{call.arguments[0].as_opt<ast::expr_handle>()}) {
                    const auto sema_type{active_mod().get_sema_type_opt(id)};
                    ASSERT(sema_type, "Cast expression must have a resolved sema type");
                    return value{emit_lvalue(*op_expr).data, *sema_type};
                }
            }

            const auto sema_type{active_mod().get_sema_type_opt(id)};
            ASSERT(sema_type, "LValue expression must have a resolved sema type");
            return lvalue_of_expr(id, *sema_type);
        },
        [&](const ast::dot_expr& dot) -> value {
            // `<Type>.member` / `@this().member` has no runtime object
            if (dot_object_is_type_namespace(dot)) {
                const auto& member_ident{active_ast().get_as<ast::identifier_expr>(dot.member)};
                if (const auto ot{active_mod().get_sema_type_opt(dot.object)}) {
                    auto* owner{ot.get()};
                    if (owner->get_kind() == sema::type_kind::TYPE) {
                        if (const auto meta{owner->get_data().as_opt<sema::types::meta_type>()}) {
                            owner = &meta->instance;
                        }
                    }
                    // A `var` static member's lvalue is its global's address.
                    if (const auto gref{try_static_member_ref(*owner, member_ident.name)}) {
                        return *gref;
                    }
                }
                const auto st{active_mod().get_sema_type_opt(id)};
                ASSERT(st, "LValue expression must have a resolved sema type");
                return lvalue_of_expr(id, *st);
            }

            auto       base_lval{emit_lvalue(dot.object)};
            const auto obj_type_opt{active_mod().get_sema_type_opt(dot.object)};
            ASSERT(obj_type_opt, "Dot expression object must have a resolved type");
            auto* obj_type{obj_type_opt.get()};

            // A reference/pointer-typed field or nested access needs one more indirection unwound
            if (const auto ref_data{obj_type->get_data().as_opt<sema::types::reference>()}) {
                auto& ref_underlying{
                    *ctx_.pool.with_const(ref_data->underlying, obj_type->is_constant())};
                base_lval.data = value::data_t{builder_.emit_load(base_lval, *obj_type)};
                base_lval.type.emplace(ref_underlying);
                obj_type = &ref_underlying;
            } else if (const auto ptr_data{obj_type->get_data().as_opt<sema::types::pointer>()}) {
                auto& ptr_underlying{
                    *ctx_.pool.with_const(ptr_data->underlying, obj_type->is_constant())};
                const value loaded_ptr{builder_.emit_load(base_lval, *obj_type), *obj_type};
                emit_null_pointer_check(loaded_ptr, id);
                base_lval.data = loaded_ptr.data;
                base_lval.type.emplace(ptr_underlying);
                obj_type = &ptr_underlying;
            }

            const auto& member_ident{active_ast().get_as<ast::identifier_expr>(dot.member)};
            if (const auto st{obj_type->get_data().as_opt<sema::types::struct_t>()};
                st && st->is_bit_packed()) {
                // A bit-packed field has no address; `&p.field` is rejected during resolution.
                UNREACHABLE("lvalue of a bit-packed struct field should be unreachable");
            }
            if (const auto ut{obj_type->get_data().as_opt<sema::types::union_t>()};
                ut && ut->is_bit_packed()) {
                UNREACHABLE("lvalue of a bit-packed union field should be unreachable");
            }
            u64 member_idx{0};
            if (obj_type->get_kind() == sema::type_kind::SLICE) {
                member_idx =
                    member_ident.name == "ptr" ? SLICE_PTR_FIELD_INDEX : SLICE_LEN_FIELD_INDEX;
            } else {
                const auto& table{ctx_.registry.get(obj_type->get_symbol_table_idx())};
                const auto  proxy{table.get_proxy_opt(member_ident.name)};
                ASSERT(proxy, "Member must exist in struct symbol table");
                member_idx = proxy->index;
            }

            auto& usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
            auto& raw_field_type{active_mod().get_sema_type_opt(dot.member).value_or(*obj_type)};
            const auto is_struct_or_union{obj_type->get_kind() == sema::type_kind::STRUCT ||
                                          obj_type->get_kind() == sema::type_kind::UNION};

            // Binding based typing
            auto& field_type{is_struct_or_union ? *ctx_.pool.with_const(
                                                      raw_field_type, base_lval.type->is_constant())
                                                : raw_field_type};

            if (const auto ut{obj_type->get_data().as_opt<sema::types::union_t>()}) {
                if (ut->is_untagged) { return value{base_lval.data, field_type}; }
                member_idx = TAGGED_UNION_PAYLOAD_INDEX;
            }

            const auto field_ptr{builder_.emit_get_element_ptr(
                base_lval, {value{member_idx, usize_type}}, field_type)};
            return value{field_ptr, field_type};
        },
        [&](const ast::index_expr& index) -> value {
            // `expr[lo..hi]` yields a fresh subslice value; spill it so callers get an address.
            if (active_ast().get_as_opt<ast::range_expr>(index.index)) {
                auto& slice_type{*active_mod().get_sema_type_opt(id)};
                return spill_to_temporary(emit_slice_range(id, index), slice_type);
            }
            auto       base_lval{emit_lvalue(index.array)};
            const auto idx_val{emit_expression(index.index)};
            const auto elem_type_opt{active_mod().get_sema_type_opt(id)};
            ASSERT(elem_type_opt, "Index expression must have a resolved element type");
            auto& elem_type{*elem_type_opt};

            const auto obj_type_opt{active_mod().get_sema_type_opt(index.array)};
            ASSERT(obj_type_opt, "Index array operand must have a resolved type");
            auto* obj_type{obj_type_opt.get()};
            bool  element_is_const{obj_type->is_constant()};
            if (const auto ref_data{obj_type->get_data().as_opt<sema::types::reference>()}) {
                auto& ref_underlying{const_cast<sema::type&>(ref_data->underlying)};
                base_lval.data = value::data_t{builder_.emit_load(base_lval, *obj_type)};
                base_lval.type.emplace(ref_underlying);
                element_is_const = obj_type->is_constant();
                obj_type         = &ref_data->underlying;
            } else if (const auto ptr_data{obj_type->get_data().as_opt<sema::types::pointer>()}) {
                auto&       ptr_underlying{const_cast<sema::type&>(ptr_data->underlying)};
                const value loaded_ptr{builder_.emit_load(base_lval, *obj_type), *obj_type};
                emit_null_pointer_check(loaded_ptr, id);
                base_lval.data = loaded_ptr.data;
                base_lval.type.emplace(ptr_underlying);
                element_is_const = obj_type->is_constant();
                obj_type         = &ptr_data->underlying;
            }

            if (const auto arr_data{obj_type->get_data().as_opt<sema::types::array>()}) {
                if (runtime_safety_) {
                    auto&      usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
                    auto&      isize_type{ctx_.get_builtin_resolved_type(sema::type_kind::ISIZE)};
                    auto&      bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
                    const bool is_signed{idx_val.type && sema::is_signed_integer(*idx_val.type)};
                    auto&      index_type{is_signed ? isize_type : usize_type};

                    value idx_cmp{idx_val};
                    if (idx_val.type && idx_val.type->get_kind() != index_type.get_kind()) {
                        const auto cast_idx{
                            builder_.emit_cast(instruction_kind::WIDEN_CAST, idx_val, index_type)};
                        idx_cmp = value{cast_idx, index_type};
                    }

                    const auto bound_val{value{static_cast<u64>(arr_data->len), index_type}};
                    const auto is_in_bounds{
                        builder_.emit_binary(instruction_kind::LT, idx_cmp, bound_val, bool_type)};

                    auto fn_opt{builder_.get_function()};
                    ASSERT(fn_opt, "Bounds check must be within an active function");
                    auto& valid_seg{fn_opt->add_segment()};
                    auto& oob_seg{fn_opt->add_segment()};

                    builder_.emit_cond_goto(
                        value{is_in_bounds, bool_type}, valid_seg.get_id(), oob_seg.get_id());

                    builder_.set_segment(oob_seg);
                    emit_panic_call("index out of bounds", id);
                    builder_.set_segment(valid_seg);
                }

                auto& write_elem_type{*ctx_.pool.with_const(elem_type, obj_type->is_constant())};
                const auto elem_ptr{
                    builder_.emit_get_element_ptr(base_lval, {idx_val}, write_elem_type)};
                return value{elem_ptr, write_elem_type};
            } else if (const auto sl_data{obj_type->get_data().as_opt<sema::types::slice>()}) {
                auto& usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
                auto& ptr_type{ctx_.get_pointer(obj_type->is_constant() ? sema::types::mut::CONSTANT
                                                                        : sema::types::mut::MUTABLE,
                                                sl_data->underlying)};

                const auto ptr_slot{builder_.emit_get_element_ptr(
                    base_lval, {value{SLICE_PTR_FIELD_INDEX, usize_type}}, ptr_type)};
                const auto ptr_val{builder_.emit_load(value{ptr_slot, ptr_type}, ptr_type)};

                if (runtime_safety_) {
                    auto& isize_type{ctx_.get_builtin_resolved_type(sema::type_kind::ISIZE)};
                    auto& bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};

                    const auto len_slot{builder_.emit_get_element_ptr(
                        base_lval, {value{SLICE_LEN_FIELD_INDEX, usize_type}}, usize_type)};
                    const auto len_val{builder_.emit_load(value{len_slot, usize_type}, usize_type)};

                    const bool is_signed{idx_val.type && sema::is_signed_integer(*idx_val.type)};
                    auto&      index_type{is_signed ? isize_type : usize_type};

                    value idx_cmp{idx_val};
                    if (idx_val.type && idx_val.type->get_kind() != index_type.get_kind()) {
                        const auto cast_idx{
                            builder_.emit_cast(instruction_kind::WIDEN_CAST, idx_val, index_type)};
                        idx_cmp = value{cast_idx, index_type};
                    }

                    value bound_val{len_val, usize_type};
                    if (is_signed) {
                        const auto cast_len{
                            builder_.emit_cast(instruction_kind::BIT_CAST, bound_val, isize_type)};
                        bound_val = value{cast_len, isize_type};
                    }

                    const auto is_in_bounds{
                        builder_.emit_binary(instruction_kind::LT, idx_cmp, bound_val, bool_type)};

                    auto fn_opt{builder_.get_function()};
                    ASSERT(fn_opt, "Bounds check must be within an active function");
                    auto& valid_seg{fn_opt->add_segment()};
                    auto& oob_seg{fn_opt->add_segment()};

                    builder_.emit_cond_goto(
                        value{is_in_bounds, bool_type}, valid_seg.get_id(), oob_seg.get_id());

                    builder_.set_segment(oob_seg);
                    emit_panic_call("index out of bounds", id);
                    builder_.set_segment(valid_seg);
                }

                auto& write_elem_type{*ctx_.pool.with_const(elem_type, obj_type->is_constant())};
                const auto elem_ptr{builder_.emit_get_element_ptr(
                    value{ptr_val, ptr_type}, {idx_val}, write_elem_type)};
                return value{elem_ptr, write_elem_type};
            }

            auto&      write_elem_type{*ctx_.pool.with_const(elem_type, element_is_const)};
            const auto elem_ptr{
                builder_.emit_get_element_ptr(base_lval, {idx_val}, write_elem_type)};
            return value{elem_ptr, write_elem_type};
        },
        [&](const ast::dereference_expr& deref) -> value {
            const auto sema_type{active_mod().get_sema_type_opt(id)};
            ASSERT(sema_type, "Dereference lvalue must have a resolved sema type");
            // The referent's own type defaults const
            const auto ptr_type{active_mod().get_sema_type_opt(*deref.rhs)};
            auto&      referent_type{
                *ctx_.pool.with_const(*sema_type, ptr_type && ptr_type->is_constant())};
            const auto raw_ptr{emit_expression_id_raw(*deref.rhs)};
            emit_null_pointer_check(raw_ptr, id);
            return value{raw_ptr.data, referent_type};
        });
}

auto emitter::emit_match(ast::node_id id, const ast::match_expr& match) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{active_mod().get_sema_type_opt(id)};
    ASSERT(sema_type, "Match expression must have a resolved sema type");
    const bool yields_value{sema_type->get_kind() != sema::type_kind::VOID_};

    // The resolver already selected an arm (`match` on a compile-time type, or `match constexpr`).
    stdx::opt_size forced_arm;
    if (const auto it{active_mod().match_arm_results.find(id.get_index())};
        it != active_mod().match_arm_results.end()) {
        forced_arm.emplace(it->second);
        // With no capture there is nothing to bind: emit only the chosen dispatch.
        if (!match.arms[it->second].capture) {
            const auto& chosen{match.arms[it->second]};
            if (yields_value) { return emit_stmt_as_value(chosen.dispatch); }
            emit_stmt(chosen.dispatch);
            return value{void_val{}, sema_type};
        }
    }

    if (const auto cv{const_eval_.try_eval(id)}) {
        if (const auto i{cv->as_int_opt()}) { return value{static_cast<i64>(*i), sema_type}; }
        if (const auto b{cv->as_opt<bool>()}) { return value{*b, sema_type}; }
        if (const auto f{cv->as_opt<f64>()}) { return value{*f, sema_type}; }
    }

    auto fn_opt{builder_.get_function()};
    ASSERT(fn_opt, "Match expression must be within an active function");
    auto& fn{*fn_opt};

    stdx::option<local_id> res_slot;
    if (yields_value) { res_slot.emplace(builder_.emit_alloca(*sema_type)); }

    const auto matcher_val{emit_expression(match.matcher)};
    auto&      bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
    auto&      merge_seg{fn.add_segment()};

    // A tagged union's tag and a capture both need the scrutinee's address
    auto& matcher_sema_type{active_mod().get_sema_type_opt(match.matcher).value_or(*sema_type)};
    const auto          union_data{matcher_sema_type.get_data().as_opt<sema::types::union_t>()};
    const bool          has_capture{std::ranges::any_of(
        match.arms, [](const ast::match_expr::arm& arm) { return arm.capture.has_value(); })};
    stdx::option<value> matcher_addr;
    if (has_capture || (union_data && !union_data->is_untagged)) {
        // Anything else is an rvalue: its value is spilled instead of re-evaluating the matcher.
        const bool is_lvalue_shape{active_ast().get_as_opt<ast::identifier_expr>(match.matcher) ||
                                   active_ast().get_as_opt<ast::dot_expr>(match.matcher) ||
                                   active_ast().get_as_opt<ast::index_expr>(match.matcher) ||
                                   active_ast().get_as_opt<ast::dereference_expr>(match.matcher)};
        if (is_lvalue_shape) {
            matcher_addr.emplace(emit_lvalue(match.matcher));
        } else {
            matcher_addr.emplace(spill_to_temporary(
                matcher_val, matcher_sema_type, matcher_sema_type.is_constant()));
        }
    }

    for (usize arm_idx{0}; arm_idx < match.arms.size(); ++arm_idx) {
        if (forced_arm && arm_idx != *forced_arm) { continue; }
        const auto& arm{match.arms[arm_idx]};
        auto&       arm_body_seg{fn.add_segment()};
        auto&       next_arm_seg{fn.add_segment()};

        const auto primary_id{*arm.primary_pattern()};
        const auto is_discard{primary_id.get_token_type() == syntax::token_type_t::UNDERSCORE ||
                              active_ast()[primary_id].template is<ast::discarded>()};

        // Test one pattern of the arm; the arm is taken if any of its patterns match.
        const auto emit_pattern_test{[&](ast::node_id pat_id) -> value {
            if (const auto range{active_ast().get_as_opt<ast::range_expr>(pat_id)}) {
                ASSERT(range->lhs && range->rhs, "A range pattern needs both endpoints");
                const auto start_val{emit_expression(*range->lhs)};
                const auto end_val{emit_expression(*range->rhs)};
                const auto ge_cond{
                    builder_.emit_binary(instruction_kind::GE, matcher_val, start_val, bool_type)};
                const auto le_kind{pat_id.get_token_type() == syntax::token_type_t::DOT_DOT_EQ
                                       ? instruction_kind::LE
                                       : instruction_kind::LT};
                const auto le_cond{builder_.emit_binary(le_kind, matcher_val, end_val, bool_type)};
                return value{builder_.emit_binary(instruction_kind::AND,
                                                  value{ge_cond, bool_type},
                                                  value{le_cond, bool_type},
                                                  bool_type),
                             bool_type};
            }
            const auto is_eq{union_data && !union_data->is_untagged
                                 ? emit_union_tag_eq(*matcher_addr, pat_id)
                                 : builder_.emit_binary(instruction_kind::EQ,
                                                        matcher_val,
                                                        emit_expression_id(pat_id),
                                                        bool_type)};
            return value{is_eq, bool_type};
        }};

        if (forced_arm || is_discard) {
            // The arm was already selected at compile time, or it is the catch-all.
            builder_.emit_goto(arm_body_seg.get_id());
        } else {
            stdx::option<value> matched;
            for (const auto& pat : arm.patterns) {
                const auto test{emit_pattern_test(*pat)};
                matched = matched ? value{builder_.emit_binary(
                                              instruction_kind::OR, *matched, test, bool_type),
                                          bool_type}
                                  : test;
            }
            builder_.emit_cond_goto(*matched, arm_body_seg.get_id(), next_arm_seg.get_id());
        }

        builder_.set_segment(arm_body_seg);
        {
            const scope_guard arm_guard{scopes_};
            // `|_|` is an anonymous capture: it consumes the arm's payload slot but binds nothing.
            if (arm.capture && arm.capture->is<ast::identifier_expr>()) {
                const auto& cap_ident{active_ast().get_as<ast::identifier_expr>(*arm.capture)};
                auto&       cap_type{
                    active_mod().get_sema_type_opt(*arm.capture).value_or(ctx_.get_int(32, true))};
                ASSERT(matcher_addr, "A capturing arm must have a computed matcher address");

                // Only a `|&v|` / `|^v|` binding aliases the field's storage
                const bool alias_capture{arm.modifier.is_ref() || arm.modifier.is_ptr()};

                // For an aliasing capture, unwrap the ref/ptr to the type addressing the storage.
                auto* underlying{&cap_type};
                if (alias_capture) {
                    if (const auto ref_d{cap_type.get_data().as_opt<sema::types::reference>()}) {
                        underlying = &const_cast<sema::type&>(ref_d->underlying);
                    } else if (const auto ptr_d{
                                   cap_type.get_data().as_opt<sema::types::pointer>()}) {
                        underlying = &const_cast<sema::type&>(ptr_d->underlying);
                    }
                }

                // An aliasing capture addresses the storage through a forced-const view so
                // it can't be written through
                auto&      qualified{alias_capture ? *ctx_.pool.with_const(*underlying, true)
                                                   : *underlying};
                value      field_addr{matcher_addr->data, qualified};
                const auto ut{matcher_addr->type->get_data().as_opt<sema::types::union_t>()};
                const bool is_tagged_union_field{ut && !ut->is_untagged};
                if (is_tagged_union_field) {
                    auto&      usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
                    const auto payload_ptr{builder_.emit_get_element_ptr(
                        *matcher_addr, {value{TAGGED_UNION_PAYLOAD_INDEX, usize_type}}, qualified)};
                    field_addr = value{payload_ptr, qualified};
                }

                if (alias_capture) {
                    // `|&v|`/`|&mut v|` and `|^v|`/`|^mut v|` alias the field's own address
                    const auto capture_slot{builder_.emit_alloca(cap_type)};
                    builder_.emit_store(capture_slot, value{field_addr.data, cap_type})
                        .is_initializer = true;
                    scopes_.back().bindings.emplace(cap_ident.name,
                                                    local_binding{
                                                        .id        = capture_slot,
                                                        .type      = cap_type,
                                                        .is_alloca = true,
                                                        .const_val = stdx::none,
                                                    });
                } else if (is_tagged_union_field) {
                    // The forced-const GEP address above already makes this read-only
                    scopes_.back().bindings.emplace(cap_ident.name,
                                                    local_binding{
                                                        .id        = field_addr.data.as<local_id>(),
                                                        .type      = *field_addr.type,
                                                        .is_alloca = true,
                                                        .const_val = stdx::none,
                                                        .is_const  = true,
                                                    });
                } else {
                    // A whole-value capture reuses the scrutinee's own storage id
                    const auto cur_val{
                        builder_.emit_load(field_addr.data.as<local_id>(), *underlying)};
                    scopes_.back().bindings.emplace(
                        cap_ident.name,
                        local_binding{local_id{0, local_kind::TEMPORARY},
                                      *underlying,
                                      false,
                                      value{cur_val, *underlying},
                                      true});
                }
            }

            if (yields_value && res_slot) {
                const auto arm_val{
                    retype_if_undefined(emit_stmt_as_value(arm.dispatch), *sema_type)};
                // A block body that diverges (`|e| { return e; }`, `_ => { return N; }`) already
                // terminated the segment; storing its `void` result would outlive the terminator.
                if (const auto seg{builder_.get_segment()}; seg && !seg->has_terminator()) {
                    builder_.emit_store(*res_slot, arm_val);
                }
            } else {
                emit_stmt(arm.dispatch);
            }
            emit_defers_for_scope(scopes_.size() - 1);
        }

        if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
            builder_.emit_goto(merge_seg.get_id());
        }

        builder_.set_segment(next_arm_seg);
    }

    if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
        builder_.emit_unreachable();
    }

    builder_.set_segment(merge_seg);
    if (yields_value && res_slot) {
        const auto loaded{builder_.emit_load(*res_slot, *sema_type)};
        return value{loaded, sema_type};
    }
    return value{void_val{}, sema_type};
}

namespace {

// A tagged union recognized as `union { ok/some: T, err/none: E }`, resolved to field ordinals.
struct unwrap_field_layout {
    u64  payload_idx;
    u64  diverge_idx;
    bool diverge_is_void;
};

[[nodiscard]] auto unwrap_layout_of(sema::context& ctx, const sema::type& union_type)
    -> unwrap_field_layout {
    const auto& table{ctx.registry.get(union_type.get_symbol_table_idx())};
    const bool  is_optional{table.get_proxy_opt("some").has_value()};
    return {
        .payload_idx     = table.get_proxy(is_optional ? "some" : "ok").index,
        .diverge_idx     = table.get_proxy(is_optional ? "none" : "err").index,
        .diverge_is_void = is_optional,
    };
}

} // namespace

auto emitter::emit_union_active_field_guard(value            union_addr,
                                            u64              field_idx,
                                            std::string_view field_name,
                                            ast::node_id     site) -> void {
    PROFILE_FUNCTION();
    if (!runtime_safety_) { return; }
    auto fn_opt{builder_.get_function()};
    if (!fn_opt) { return; }
    auto& fn{*fn_opt};

    auto& i32_type{ctx_.get_int(32, true)};
    auto& usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
    auto& bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};

    const auto tag_ptr{builder_.emit_get_element_ptr(
        union_addr, {value{TAGGED_UNION_DISCRIMINANT_INDEX, usize_type}}, i32_type)};
    const auto tag_val{builder_.emit_load(value{tag_ptr, i32_type}, i32_type)};
    const auto is_active{builder_.emit_binary(instruction_kind::EQ,
                                              value{tag_val, i32_type},
                                              value{static_cast<i64>(field_idx), i32_type},
                                              bool_type)};

    auto& active_seg{fn.add_segment()};
    auto& inactive_seg{fn.add_segment()};
    builder_.emit_cond_goto(
        value{is_active, bool_type}, active_seg.get_id(), inactive_seg.get_id());

    builder_.set_segment(inactive_seg);
    emit_panic_call(fmt::format("accessed inactive union field '{}'", field_name), site);

    builder_.set_segment(active_seg);
}

auto emitter::emit_unwrap(ast::node_id id, const ast::unwrap_expr& unwrap) -> value {
    PROFILE_FUNCTION();
    const auto payload_type_opt{active_mod().get_sema_type_opt(id)};
    ASSERT(payload_type_opt, "unwrap expression must have a resolved payload type");
    auto& payload_type{*payload_type_opt};

    const auto operand_type_opt{active_mod().get_sema_type_opt(unwrap.operand)};
    ASSERT(operand_type_opt, "unwrap operand must have a resolved type");
    auto& operand_type{*operand_type_opt};
    ASSERT(operand_type.get_data().as_opt<sema::types::union_t>(),
           "unwrap operand must be a tagged union");

    const auto layout{unwrap_layout_of(ctx_, operand_type)};

    auto fn_opt{builder_.get_function()};
    ASSERT(fn_opt, "unwrap must be within an active function");
    [[maybe_unused]] auto& fn{*fn_opt};

    [[maybe_unused]] auto& i32_type{ctx_.get_int(32, true)};
    auto&                  usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
    [[maybe_unused]] auto& bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};

    // Address of the scrutinee: reuse its storage when it is an lvalue, else spill the rvalue.
    const bool is_lvalue_shape{active_ast().get_as_opt<ast::identifier_expr>(unwrap.operand) ||
                               active_ast().get_as_opt<ast::dot_expr>(unwrap.operand) ||
                               active_ast().get_as_opt<ast::index_expr>(unwrap.operand) ||
                               active_ast().get_as_opt<ast::dereference_expr>(unwrap.operand)};
    const auto operand_addr{is_lvalue_shape ? emit_lvalue(unwrap.operand)
                                            : spill_to_temporary(emit_expression(unwrap.operand),
                                                                 operand_type,
                                                                 operand_type.is_constant())};

    // `?` propagation is control flow, not a safety check, so it is always emitted
    const bool is_propagation{id.get_token_type() == syntax::token_type_t::QUESTION};
    if (is_propagation || runtime_safety_) {
        const auto tag_ptr{builder_.emit_get_element_ptr(
            operand_addr, {value{TAGGED_UNION_DISCRIMINANT_INDEX, usize_type}}, i32_type)};
        const auto tag_val{builder_.emit_load(value{tag_ptr, i32_type}, i32_type)};
        const auto is_payload{
            builder_.emit_binary(instruction_kind::EQ,
                                 value{tag_val, i32_type},
                                 value{static_cast<i64>(layout.payload_idx), i32_type},
                                 bool_type)};

        auto& payload_seg{fn.add_segment()};
        auto& diverge_seg{fn.add_segment()};
        builder_.emit_cond_goto(
            value{is_payload, bool_type}, payload_seg.get_id(), diverge_seg.get_id());

        builder_.set_segment(diverge_seg);
        if (is_propagation) {
            emit_unwrap_propagation(
                operand_addr, operand_type, layout.diverge_idx, layout.diverge_is_void, id);
        } else {
            emit_panic_call(layout.diverge_is_void ? "'!' unwrapped an empty optional"
                                                   : "'!' unwrapped an errored result",
                            id);
        }

        // `diverge_seg` always terminates; execution continues in `payload_seg`.
        builder_.set_segment(payload_seg);
    }

    const auto payload_ptr{builder_.emit_get_element_ptr(
        operand_addr, {value{TAGGED_UNION_PAYLOAD_INDEX, usize_type}}, payload_type)};
    const auto loaded{builder_.emit_load(value{payload_ptr, payload_type}, payload_type)};
    return value{loaded, payload_type};
}

auto emitter::emit_unwrap_propagation(value             operand_addr,
                                      const sema::type& operand_union,
                                      u64               operand_diverge_idx,
                                      bool              diverge_is_void,
                                      ast::node_id      site) -> void {
    PROFILE_FUNCTION();
    auto fn_opt{builder_.get_function()};
    ASSERT(fn_opt, "`?` propagation must be within an active function");
    const auto fn_data{fn_opt->get_type().get_data().as_opt<sema::types::function>()};
    ASSERT(fn_data, "`?` propagation requires the enclosing function signature");
    auto& ret_type{fn_data->return_type};

    const auto ret_layout{unwrap_layout_of(ctx_, ret_type)};

    auto& i32_type{ctx_.get_int(32, true)};
    auto& usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};

    builder_.set_location(active_ast().location_of(site));
    const auto ret_slot{builder_.emit_alloca(ret_type)};

    const auto tag_ptr{builder_.emit_get_element_ptr(
        value{ret_slot, ret_type}, {value{TAGGED_UNION_DISCRIMINANT_INDEX, usize_type}}, i32_type)};
    builder_
        .emit_store(value{tag_ptr, i32_type},
                    value{static_cast<i64>(ret_layout.diverge_idx), i32_type})
        .is_initializer = true;

    // A non-void divergent payload is copied across
    if (!diverge_is_void) {
        auto& src_type{operand_union.get_data().as<sema::types::union_t>().type_at(
            static_cast<usize>(operand_diverge_idx))};
        auto& dst_type{ret_type.get_data().as<sema::types::union_t>().type_at(
            static_cast<usize>(ret_layout.diverge_idx))};

        const auto src_ptr{builder_.emit_get_element_ptr(
            operand_addr, {value{TAGGED_UNION_PAYLOAD_INDEX, usize_type}}, src_type)};
        const auto err_val{builder_.emit_load(value{src_ptr, src_type}, src_type)};

        const auto dst_ptr{builder_.emit_get_element_ptr(
            value{ret_slot, ret_type}, {value{TAGGED_UNION_PAYLOAD_INDEX, usize_type}}, dst_type)};
        builder_.emit_store(value{dst_ptr, dst_type}, value{err_val, src_type}).is_initializer =
            true;
    }

    const auto ret_val{builder_.emit_load(value{ret_slot, ret_type}, ret_type)};
    emit_defers_up_to(0);
    builder_.set_location(active_ast().location_of(site));
    builder_.emit_return(value{ret_val, ret_type});
}

auto emitter::emit_initializer(ast::node_id id, const ast::initializer_expr& init) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{active_mod().get_sema_type_opt(id)};
    ASSERT(sema_type, "Initializer expression must have a resolved sema type");

    if (const auto cv{const_eval_.try_eval(id)}) {
        if (const auto i{cv->as_int_opt()}) { return value{static_cast<i64>(*i), sema_type}; }
        if (const auto b{cv->as_opt<bool>()}) { return value{*b, sema_type}; }
        if (const auto f{cv->as_opt<f64>()}) { return value{*f, sema_type}; }
    }

    const auto struct_slot{builder_.emit_alloca(*sema_type)};
    auto&      usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};

    // `RowAlias{ a, b, c }`: an array literal of positional values.
    if (const auto arr{sema_type->get_data().as_opt<sema::types::array>()}) {
        auto& elem_type{arr->underlying};
        for (u64 i{0}; const auto& [accessor, val_expr] : init.initializers) {
            const auto elem_ptr{builder_.emit_get_element_ptr(
                value{struct_slot, *sema_type}, {value{i, usize_type}}, elem_type)};
            const auto val{emit_coerced_expr(val_expr, elem_type)};
            builder_.emit_store(value{elem_ptr, elem_type}, val).is_initializer = true;
            ++i;
        }
        const auto loaded{builder_.emit_load(value{struct_slot, *sema_type}, *sema_type)};
        return value{loaded, sema_type};
    }

    if (const auto ut{sema_type->get_data().as_opt<sema::types::union_t>()}) {
        if (ut->is_bit_packed()) {
            // Backing integer: zero it, then OR the single set field in at bit offset 0.
            const auto  n{sema::packed_union_backing_bits(*ut, target_ptr_bits_).value_or(1)};
            auto&       backing_ty{ctx_.get_int(static_cast<u16>(n), false)};
            const auto& table{ctx_.registry.get(sema_type->get_symbol_table_idx())};
            builder_.emit_store(value{struct_slot, backing_ty}, value{u64{0}, backing_ty})
                .is_initializer = true;
            for (const auto& [accessor, val_expr] : init.initializers) {
                const auto& imp{active_ast().get_as<ast::implicit_access_expr>(*accessor)};
                const auto& name{active_ast().get_as<ast::identifier_expr>(imp.member).name};
                const auto  proxy{table.get_proxy_opt(name)};
                ASSERT(proxy, "Member must exist in union symbol table");
                auto&      field_type{ut->type_at(proxy->index)};
                const auto val{emit_coerced_expr(val_expr, field_type)};
                emit_packed_field_write(value{struct_slot, backing_ty}, *ut, field_type, val);
            }
            const auto loaded{builder_.emit_load(value{struct_slot, *sema_type}, *sema_type)};
            return value{loaded, sema_type};
        }

        if (ut->is_untagged) {
            for (const auto& [accessor, val_expr] : init.initializers) {
                auto& field_type{active_mod().get_sema_type_opt(*val_expr).value_or(*sema_type)};
                const auto val{emit_coerced_expr(val_expr, field_type)};
                builder_.emit_store(value{struct_slot, field_type}, val);
            }
            const auto loaded{builder_.emit_load(value{struct_slot, *sema_type}, *sema_type)};
            return value{loaded, sema_type};
        }

        ASSERT(init.initializers.size() == 1,
               "Tagged union initializer must set exactly one field");
        const auto& [accessor, val_expr]{init.initializers[0]};
        const auto& table{ctx_.registry.get(sema_type->get_symbol_table_idx())};
        const auto& imp{active_ast().get_as<ast::implicit_access_expr>(*accessor)};
        const auto& name{active_ast().get_as<ast::identifier_expr>(imp.member).name};
        const auto  proxy{table.get_proxy_opt(name)};
        ASSERT(proxy, "Member must exist in union symbol table");
        const auto [sym, field_idx]{*proxy};

        auto&      i32_type{ctx_.get_int(32, true)};
        const auto tag_ptr{
            builder_.emit_get_element_ptr(value{struct_slot, *sema_type},
                                          {value{TAGGED_UNION_DISCRIMINANT_INDEX, usize_type}},
                                          i32_type)};
        builder_.emit_store(value{tag_ptr, i32_type}, value{static_cast<i64>(field_idx), i32_type})
            .is_initializer = true;

        auto&      field_type{ut->type_at(field_idx)};
        const auto payload_ptr{
            builder_.emit_get_element_ptr(value{struct_slot, *sema_type},
                                          {value{TAGGED_UNION_PAYLOAD_INDEX, usize_type}},
                                          field_type)};
        const auto val{emit_coerced_expr(val_expr, field_type)};
        builder_.emit_store(value{payload_ptr, field_type}, val).is_initializer = true;

        const auto loaded{builder_.emit_load(value{struct_slot, *sema_type}, *sema_type)};
        return value{loaded, sema_type};
    }

    const auto st{sema_type->get_data().as_opt<sema::types::struct_t>()};
    ASSERT(st, "Initializer target must be a struct type");
    const auto& table{ctx_.registry.get(sema_type->get_symbol_table_idx())};

    // A bit-packed struct is a single backing integer: zero it, then OR each field instead of GEP
    if (st->is_bit_packed()) {
        const auto n{sema::packed_backing_bits(*st, target_ptr_bits_).value_or(1)};
        auto&      backing_ty{ctx_.get_int(static_cast<u16>(n), false)};
        builder_.emit_store(value{struct_slot, backing_ty}, value{u64{0}, backing_ty})
            .is_initializer = true;
        for (const auto& [accessor, val_expr] : init.initializers) {
            const auto& imp{active_ast().get_as<ast::implicit_access_expr>(*accessor)};
            const auto& name{active_ast().get_as<ast::identifier_expr>(imp.member).name};
            const auto  proxy{table.get_proxy_opt(name)};
            ASSERT(proxy, "Member must exist in struct symbol table");
            const auto [sym, field_idx]{*proxy};
            auto&      field_type{st->type_at(field_idx)};
            const auto val{emit_coerced_expr(val_expr, field_type)};
            emit_packed_field_write(
                value{struct_slot, backing_ty}, *st, field_idx, field_type, val);
        }
        const auto loaded{builder_.emit_load(value{struct_slot, *sema_type}, *sema_type)};
        return value{loaded, sema_type};
    }

    for (const auto& [accessor, val_expr] : init.initializers) {
        const auto& imp{active_ast().get_as<ast::implicit_access_expr>(*accessor)};
        const auto& name{active_ast().get_as<ast::identifier_expr>(imp.member).name};
        const auto  proxy{table.get_proxy_opt(name)};
        ASSERT(proxy, "Member must exist in struct symbol table");
        const auto [sym, field_idx]{*proxy};
        auto&      field_type{st->type_at(field_idx)};
        const auto field_ptr{
            builder_.emit_get_element_ptr(value{struct_slot, *sema_type},
                                          {value{static_cast<u64>(field_idx), usize_type}},
                                          field_type)};
        const auto val{emit_coerced_expr(val_expr, field_type)};
        builder_.emit_store(value{field_ptr, field_type}, val).is_initializer = true;
    }

    const auto loaded{builder_.emit_load(value{struct_slot, *sema_type}, *sema_type)};
    return value{loaded, sema_type};
}

// True when `dot.object` denotes a type used purely as a namespace
auto emitter::dot_object_is_type_namespace(const ast::dot_expr& dot) -> bool {
    if (const auto ot{active_mod().get_sema_type_opt(dot.object)};
        ot && ot->get_kind() == sema::type_kind::TYPE) {
        return true;
    }
    if (const auto call{active_ast().get_as_opt<ast::call_expr>(dot.object)}) {
        if (const auto fi{active_ast().get_as_opt<ast::identifier_expr>(call->function)}) {
            return fi->name == "@this";
        }
    }
    if (const auto oi{active_ast().get_as_opt<ast::identifier_expr>(dot.object)}) {
        if (lookup_binding(oi->name)) { return false; }
        if (const auto rt{active_mod().root_table_idx}) {
            if (const auto s{ctx_.registry.get_from_opt(*rt, oi->name)}) {
                return s->has_kind() && s->get_kind() == sema::symbol_kind::TYPE;
            }
        }
    }
    return false;
}

auto emitter::emit_dot(ast::node_id id, const ast::dot_expr& dot) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{active_mod().get_sema_type_opt(id)};
    const auto raw_obj_type{active_mod().get_sema_type_opt(dot.object)};
    ASSERT(raw_obj_type, "Dot expression object must have a resolved type");
    auto* obj_type{raw_obj_type.get()};

    if (dot_object_is_type_namespace(dot)) {
        if (const auto cv{const_eval_.try_eval(id)}) {
            if (const auto i{cv->as_int_opt()}) { return value{static_cast<i64>(*i), sema_type}; }
            if (const auto b{cv->as_opt<bool>()}) { return value{*b, sema_type}; }
            if (const auto f{cv->as_opt<f64>()}) { return value{*f, sema_type}; }
            if (cv->is<const_struct>() || cv->is<const_array>() || cv->is<const_union>() ||
                cv->is<std::string>()) {
                return materialize_const(*cv);
            }
        }
        const auto& member_ident{active_ast().get_as<ast::identifier_expr>(dot.member)};
        // A `var` static member has real storage: load it from its global.
        auto* owner{obj_type};
        if (owner->get_kind() == sema::type_kind::TYPE) {
            if (const auto meta{owner->get_data().as_opt<sema::types::meta_type>()}) {
                owner = &meta->instance;
            }
        }
        if (const auto gref{try_static_member_ref(*owner, member_ident.name)}) {
            auto& gtype{const_cast<sema::type&>(*gref->type)};
            return value{builder_.emit_load(*gref, gtype), gtype};
        }
        return value{ref_symbol_name(id, member_ident.name), sema_type};
    }

    // A directly-held bit-packed struct value has no addressable fields
    if (const auto st{obj_type->get_data().as_opt<sema::types::struct_t>()};
        st && st->is_bit_packed()) {
        const auto& member_ident{active_ast().get_as<ast::identifier_expr>(dot.member)};
        const auto& table{ctx_.registry.get(obj_type->get_symbol_table_idx())};
        const auto  proxy{table.get_proxy_opt(member_ident.name)};
        ASSERT(proxy, "Member must exist in bit-packed struct symbol table");
        const auto  n{sema::packed_backing_bits(*st, target_ptr_bits_).value_or(1)};
        auto&       backing_ty{ctx_.get_int(static_cast<u16>(n), false)};
        const value backing{emit_expression(dot.object).data, backing_ty};
        auto&       field_type{sema_type ? *sema_type : const_cast<sema::type&>(*obj_type)};
        return emit_packed_field_extract(backing, *st, proxy->index, field_type);
    }
    if (const auto ut{obj_type->get_data().as_opt<sema::types::union_t>()};
        ut && ut->is_bit_packed()) {
        const auto n{sema::packed_union_backing_bits(*ut, target_ptr_bits_).value_or(1)};
        const auto fbits{
            sema::packed_field_bits(sema_type ? *sema_type : *obj_type, target_ptr_bits_)
                .value_or(n)};
        auto&       backing_ty{ctx_.get_int(static_cast<u16>(n), false)};
        const value backing{emit_expression(dot.object).data, backing_ty};
        auto&       field_type{sema_type ? *sema_type : const_cast<sema::type&>(*obj_type)};
        return emit_packed_bits_extract(backing, n, 0, fbits, field_type);
    }

    // emit_lvalue(dot.object) addresses the object's own storage; still needs unwinding here
    auto base_lval{emit_lvalue(dot.object)};
    if (const auto ptr_data{obj_type->get_data().as_opt<sema::types::pointer>()}) {
        const value loaded_ptr{builder_.emit_load(base_lval, *obj_type), *obj_type};
        emit_null_pointer_check(loaded_ptr, id);
        base_lval.data = loaded_ptr.data;
        base_lval.type.emplace(ptr_data->underlying);
        obj_type = &ptr_data->underlying;
    } else if (const auto ref_data{obj_type->get_data().as_opt<sema::types::reference>()}) {
        base_lval.data = value::data_t{builder_.emit_load(base_lval, *obj_type)};
        base_lval.type.emplace(ref_data->underlying);
        obj_type = &ref_data->underlying;
    }

    const auto member_ident{active_ast().get_as<ast::identifier_expr>(dot.member)};

    // The thunk itself is static, so this ignores base_lval entirely
    if (obj_type->get_kind() == sema::type_kind::CLOSURE && member_ident.name == "thunk") {
        return value{fmt::format("closure{}", obj_type->get_symbol_table_idx()), sema_type};
    }

    if (obj_type->get_kind() == sema::type_kind::SLICE) {
        const u64  member_idx{member_ident.name == "ptr" ? SLICE_PTR_FIELD_INDEX
                                                         : SLICE_LEN_FIELD_INDEX};
        auto&      usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
        auto&      field_type{sema_type ? *sema_type : *obj_type};
        const auto field_ptr{
            builder_.emit_get_element_ptr(base_lval, {value{member_idx, usize_type}}, field_type)};
        const auto loaded{builder_.emit_load(value{field_ptr, field_type}, field_type)};
        return value{loaded, field_type};
    }

    if (obj_type->get_kind() == sema::type_kind::ARRAY) {
        const auto arr_data{obj_type->get_data().as_opt<sema::types::array>()};
        auto&      usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
        if (member_ident.name == "len") {
            return value{static_cast<u64>(arr_data->len), usize_type};
        }

        if (member_ident.name == "ptr") {
            auto&      field_type{sema_type ? *sema_type : arr_data->underlying};
            const auto field_ptr{builder_.emit_get_element_ptr(
                base_lval, {value{ARRAY_PTR_FIELD_INDEX, usize_type}}, field_type)};
            return value{field_ptr, field_type};
        }
    }

    const auto& table{ctx_.registry.get(obj_type->get_symbol_table_idx())};
    if (const auto proxy{table.get_proxy_opt(member_ident.name)}) {
        const auto [sym, member_idx]{*proxy};
        auto& usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};

        // Fields come first in the symbol table; anything past them is a member declaration
        if (const auto st{obj_type->get_data().as_opt<sema::types::struct_t>()};
            st && member_idx < st->fields.size()) {
            auto& field_type{sema_type ? *sema_type : *obj_type};
            if (st->is_bit_packed()) {
                return emit_packed_field_read(base_lval, *st, member_idx, field_type);
            }
            const auto field_ptr{builder_.emit_get_element_ptr(
                base_lval, {value{static_cast<u64>(member_idx), usize_type}}, field_type)};
            const auto loaded{builder_.emit_load(value{field_ptr, field_type}, field_type)};
            return value{loaded, field_type};
        }

        if (const auto ut{obj_type->get_data().as_opt<sema::types::union_t>()};
            ut && member_idx < ut->fields.size()) {
            auto& field_type{sema_type ? *sema_type : *obj_type};
            if (ut->is_bit_packed()) { return emit_packed_field_read(base_lval, *ut, field_type); }
            if (ut->is_untagged) {
                const auto loaded{
                    builder_.emit_load(value{base_lval.data, field_type}, field_type)};
                return value{loaded, field_type};
            }

            emit_union_active_field_guard(
                base_lval, static_cast<u64>(member_idx), member_ident.name, id);

            const auto payload_ptr{builder_.emit_get_element_ptr(
                base_lval, {value{TAGGED_UNION_PAYLOAD_INDEX, usize_type}}, field_type)};
            const auto loaded{builder_.emit_load(value{payload_ptr, field_type}, field_type)};
            return value{loaded, field_type};
        }
    }

    if (const auto cv{const_eval_.try_eval(id)}) {
        if (const auto i{cv->as_int_opt()}) { return value{static_cast<i64>(*i), sema_type}; }
        if (const auto b{cv->as_opt<bool>()}) { return value{*b, sema_type}; }
        if (const auto f{cv->as_opt<f64>()}) { return value{*f, sema_type}; }
        if (cv->is<const_struct>() || cv->is<const_array>() || cv->is<const_union>() ||
            cv->is<std::string>()) {
            return materialize_const(*cv);
        }
    }

    return value{ref_symbol_name(id, member_ident.name), sema_type};
}

auto emitter::emit_index(ast::node_id id, const ast::index_expr& index) -> value {
    PROFILE_FUNCTION();
    if (active_ast().get_as_opt<ast::range_expr>(index.index)) {
        return emit_slice_range(id, index);
    }
    const auto sema_type{active_mod().get_sema_type_opt(id)};
    ASSERT(sema_type, "Index expression must have a resolved sema type");
    const auto elem_lval{emit_lvalue(id)};
    const auto loaded{builder_.emit_load(elem_lval, *sema_type)};
    return value{loaded, sema_type};
}

auto emitter::emit_slice_range(ast::node_id id, const ast::index_expr& index) -> value {
    PROFILE_FUNCTION();
    const auto result_type{active_mod().get_sema_type_opt(id)};
    ASSERT(result_type && result_type->get_kind() == sema::type_kind::SLICE,
           "A range index must resolve to a slice type");
    const auto slice_data{result_type->get_data().as_opt<sema::types::slice>()};
    ASSERT(slice_data, "Slice sema type must carry slice data");
    auto& elem_type{const_cast<sema::type&>(slice_data->underlying)};

    auto& usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
    auto& bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
    auto& ptr_type{ctx_.get_pointer(result_type->is_constant() ? sema::types::mut::CONSTANT
                                                               : sema::types::mut::MUTABLE,
                                    elem_type)};

    const auto& range{active_ast().get_as<ast::range_expr>(index.index)};
    const bool  inclusive{ast::node_id{index.index}.get_token_type() ==
                         syntax::token_type_t::DOT_DOT_EQ};

    // An omitted lower bound is `0`; an omitted upper bound is the source length, filled in once
    // it is known below.
    const auto lo{range.lhs ? emit_coerced_expr(*range.lhs, usize_type)
                            : value{u64{0}, usize_type}};

    // Base element pointer and (for arrays / slices) the source length.
    auto  src_lval{emit_lvalue(index.array)};
    auto* src_type{active_mod().get_sema_type_opt(index.array).get()};
    if (const auto ref_d{src_type->get_data().as_opt<sema::types::reference>()}) {
        src_lval.data = value::data_t{builder_.emit_load(src_lval, *src_type)};
        src_type      = &const_cast<sema::type&>(ref_d->underlying);
    }

    value               base_ptr{};
    stdx::option<value> src_len;
    if (const auto arr_d{src_type->get_data().as_opt<sema::types::array>()}) {
        base_ptr =
            value{builder_.emit_get_element_ptr(src_lval, {value{u64{0}, usize_type}}, ptr_type),
                  ptr_type};
        src_len.emplace(value{static_cast<u64>(arr_d->len), usize_type});
    } else if (src_type->get_data().as_opt<sema::types::slice>()) {
        const auto ptr_slot{builder_.emit_get_element_ptr(
            src_lval, {value{SLICE_PTR_FIELD_INDEX, usize_type}}, ptr_type)};
        base_ptr = value{builder_.emit_load(value{ptr_slot, ptr_type}, ptr_type), ptr_type};
        const auto len_slot{builder_.emit_get_element_ptr(
            src_lval, {value{SLICE_LEN_FIELD_INDEX, usize_type}}, usize_type)};
        src_len.emplace(
            value{builder_.emit_load(value{len_slot, usize_type}, usize_type), usize_type});
    } else if (src_type->get_data().as_opt<sema::types::pointer>()) {
        const value loaded_ptr{builder_.emit_load(src_lval, *src_type), *src_type};
        emit_null_pointer_check(loaded_ptr, id);
        base_ptr = value{loaded_ptr.data, ptr_type};
    } else {
        UNREACHABLE("Range index source must be an array, slice, or pointer");
    }

    value hi{};
    if (range.rhs) {
        hi = emit_coerced_expr(*range.rhs, usize_type);
        if (inclusive) {
            hi = value{builder_.emit_binary(
                           instruction_kind::ADD, hi, value{u64{1}, usize_type}, usize_type),
                       usize_type};
        }
    } else {
        ASSERT(src_len, "An open-ended `..` upper bound needs a known source length");
        hi = *src_len;
    }

    // Bounds check: lo <= hi, and hi <= len when the source length is known.
    if (runtime_safety_) {
        auto fn_opt{builder_.get_function()};
        ASSERT(fn_opt, "Slice range must be within an active function");
        auto&      fn{*fn_opt};
        const auto lo_le_hi{builder_.emit_binary(instruction_kind::LE, lo, hi, bool_type)};
        value      in_bounds{lo_le_hi, bool_type};
        if (src_len) {
            const auto hi_le_len{
                builder_.emit_binary(instruction_kind::LE, hi, *src_len, bool_type)};
            in_bounds =
                value{builder_.emit_binary(
                          instruction_kind::AND, in_bounds, value{hi_le_len, bool_type}, bool_type),
                      bool_type};
        }
        auto& ok_seg{fn.add_segment()};
        auto& oob_seg{fn.add_segment()};
        builder_.emit_cond_goto(in_bounds, ok_seg.get_id(), oob_seg.get_id());
        builder_.set_segment(oob_seg);
        emit_panic_call("slice range out of bounds", id);
        builder_.set_segment(ok_seg);
    }

    const auto new_ptr{builder_.emit_get_element_ptr(base_ptr, {lo}, ptr_type)};
    const auto new_len{builder_.emit_binary(instruction_kind::SUB, hi, lo, usize_type)};

    const auto slot{builder_.emit_alloca(*result_type)};
    const auto f0{builder_.emit_get_element_ptr(
        value{slot, *result_type}, {value{SLICE_PTR_FIELD_INDEX, usize_type}}, ptr_type)};
    builder_.emit_store(value{f0, ptr_type}, value{new_ptr, ptr_type}).is_initializer = true;
    const auto f1{builder_.emit_get_element_ptr(
        value{slot, *result_type}, {value{SLICE_LEN_FIELD_INDEX, usize_type}}, usize_type)};
    builder_.emit_store(value{f1, usize_type}, value{new_len, usize_type}).is_initializer = true;

    return value{builder_.emit_load(value{slot, *result_type}, *result_type), result_type};
}

auto emitter::emit_address_of(ast::node_id id, const ast::address_of_expr& addr) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{active_mod().get_sema_type_opt(id)};
    ASSERT(sema_type, "Address of expression must have a resolved sema type");

    // `^r` on a reference aliases the referent, cannot have a pointer to a reference
    if (const auto rhs_type{active_mod().get_sema_type_opt(*addr.rhs)};
        rhs_type && rhs_type->get_kind() == sema::type_kind::REFERENCE) {
        const auto ref_val{emit_expression_id_raw(*addr.rhs)};
        return value{ref_val.data, sema_type};
    }

    const auto target{emit_lvalue(addr.rhs)};
    auto&      res_type{*sema_type};
    const auto ptr{builder_.emit_address_of(target, res_type)};
    return value{ptr, sema_type};
}

auto emitter::emit_dereference(ast::node_id id, const ast::dereference_expr& deref) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{active_mod().get_sema_type_opt(id)};
    ASSERT(sema_type, "Dereference expression must have a resolved sema type");
    const auto ptr_val{emit_expression_id_raw(*deref.rhs)};
    auto&      elem_type{*sema_type};
    emit_null_pointer_check(ptr_val, id);
    const auto loaded{builder_.emit_load(ptr_val, elem_type)};
    return value{loaded, sema_type};
}

auto emitter::emit_reference(ast::node_id id, const ast::reference_expr& ref) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{active_mod().get_sema_type_opt(id)};
    ASSERT(sema_type, "Reference expression must have a resolved sema type");
    const auto target{emit_lvalue(ref.rhs)};
    auto&      res_type{*sema_type};
    const auto ptr{builder_.emit_address_of(target, res_type)};
    return value{ptr, sema_type};
}

auto emitter::emit_implicit_access(ast::node_id id, const ast::implicit_access_expr& imp) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{active_mod().get_sema_type_opt(id)};
    if (const auto cv{const_eval_.try_eval(id)}) { return cv->to_gir_value(); }
    const auto& ident{active_ast().get_as<ast::identifier_expr>(imp.member)};
    return value{ref_symbol_name(id, ident.name), sema_type};
}

auto emitter::emit_module_access(ast::node_id id, const ast::module_access_expr& mod_access)
    -> value {
    PROFILE_FUNCTION();
    const auto sema_type{active_mod().get_sema_type_opt(id)};
    if (const auto cv{const_eval_.try_eval(id)}) { return cv->to_gir_value(); }
    const auto& inner_ident{active_ast().get_as<ast::identifier_expr>(mod_access.inner)};
    return value{ref_symbol_name(id, inner_ident.name), sema_type};
}

} // namespace ghoti::gir
