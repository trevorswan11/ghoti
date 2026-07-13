#include "compiler/gir/emitter.hh"

#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
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
#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/gir/segment.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/generic.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/token_type.hh"

namespace ghoti::gir {

auto emitter::emit() -> module {
    PROFILE_FUNCTION();
    const_eval_.resolve_all_deferred_types();

    for (const auto root_id : ast_module_.ast) {
        ast_module_.ast[root_id].visit(
            [&](const auto&) {},
            [&](const ast::decl_stmt& decl) { emit_top_level_decl(root_id, decl); },
            [&](const ast::using_stmt& using_stmt) { emit_top_level_using(root_id, using_stmt); },
            [&](const ast::test_stmt& test) { emit_top_level_test(root_id, test); });
    }

    for (const auto& req : ast_module_.generic_instantiations) { emit_generic_instantiation(req); }
    return std::move(gir_module_);
}

auto emitter::emit_generic_instantiation(const sema::generic_instantiation_request& req) -> void {
    PROFILE_FUNCTION();
    const auto fn_expr_opt = ast_module_.ast[req.fn_node_id].visit(
        [&](const auto&) -> stdx::option<const ast::function_expr&> { return stdx::none; },
        [&](const ast::decl_stmt& decl) -> stdx::option<const ast::function_expr&> {
            if (decl.value) { return ast_module_.ast.get_as_opt<ast::function_expr>(*decl.value); }
            return stdx::none;
        },
        [&](const ast::function_expr& fn_expr) -> stdx::option<const ast::function_expr&> {
            return fn_expr;
        });

    VERIFY(fn_expr_opt, "Generic instantiation must reference a valid function expression");
    const auto& fn_expr{*fn_expr_opt};

    auto& fn{gir_module_.add_function(req.mangled_name, *req.return_type, false, false)};
    auto& entry{fn.add_segment()};
    builder_.set_insert_point(fn, entry);

    const scope_guard g{scopes_};
    for (const auto& [param, arg_type] : std::views::zip(fn_expr.parameters, req.arg_types)) {
        const auto& p_ident{ast_module_.ast.get_as<ast::identifier_expr>(param.name)};
        const auto  p_name{p_ident.name};

        auto& p_slot{fn.add_param(std::string{p_name}, *arg_type)};
        scopes_.back().bindings.emplace(p_name,
                                        local_binding{p_slot.id, *arg_type, false, stdx::none});
    }

    emit_block(ast_module_.ast.get_as<ast::block_stmt>(fn_expr.body));
    if (const auto cur_seg{builder_.get_segment()}; !cur_seg->has_terminator()) {
        if (req.return_type->get_kind() == sema::type_kind::VOID) {
            builder_.emit_return();
        } else {
            builder_.emit_return(value{undefined_val{}, *req.return_type});
        }
    }
}

auto emitter::emit_top_level_decl(ast::node_id id, const ast::decl_stmt& decl) -> void {
    PROFILE_FUNCTION();
    const auto& name_ident{ast_module_.ast.get_as<ast::identifier_expr>(decl.name)};
    const auto  name{name_ident.name};
    const auto  sema_type{ast_module_.get_sema_type_opt(id)};
    ASSERT(sema_type, "Top-level declaration must have a resolved sema type");

    if (decl.value) {
        if (const auto fn_expr{ast_module_.ast.get_as_opt<ast::function_expr>(*decl.value)}) {
            if (const auto fn_data{sema_type->get_data().as_opt<sema::types::function>()}) {
                // Generic templates will be emitted via monomorphized instantiations
                if (std::ranges::contains(
                        fn_data->params, sema::type_kind::AUTO, &sema::type::get_kind)) {
                    return;
                }
            }
            return emit_function(id, decl, *fn_expr);
        }
    } else if (decl.has_modifier(ast::decl_modifiers::EXTERN)) {
        if (const auto fn_data{sema_type->get_data().as_opt<sema::types::function>()}) {
            auto& fn{gir_module_.add_function(
                std::string{name}, *sema_type, false, false, fn_data->is_variadic)};
            for (usize i{0}; const auto& param : fn_data->params) {
                fn.add_param(fmt::format("param.{}", i++), *param);
            }
            return;
        }
    }

    const auto is_const{decl.has_modifier(ast::decl_modifiers::CONSTANT) ||
                        decl.has_modifier(ast::decl_modifiers::CONSTEXPR)};

    if (is_const) {
        stdx::option<value> init_val;
        if (decl.value) {
            if (const auto cv{const_eval_.try_eval(*decl.value)}) {
                init_val.emplace(cv->to_gir_value());
            }
        }
        gir_module_.add_global(std::string{name}, *sema_type, true, init_val);
    } else {
        stdx::option<value> init_val;
        if (decl.value) {
            if (const auto cv{const_eval_.try_eval(*decl.value)}) {
                init_val.emplace(cv->to_gir_value());
            } else {
                init_val.emplace(emit_expression(*decl.value));
            }
        }
        gir_module_.add_global(std::string{name}, *sema_type, false, init_val);
    }
}

auto emitter::emit_top_level_using(ast::node_id, const ast::using_stmt& using_stmt) -> void {
    PROFILE_FUNCTION();
    const auto& name_ident{ast_module_.ast.get_as<ast::identifier_expr>(using_stmt.alias)};
    const auto  sema_type{ast_module_.get_sema_type_opt(using_stmt.explicit_type)};
    ASSERT(sema_type, "Using statement explicit type must be resolved");
    gir_module_.add_type(std::string{name_ident.name}, *sema_type);
}

auto emitter::emit_top_level_test(ast::node_id, const ast::test_stmt& test) -> void {
    PROFILE_FUNCTION();
    auto&      void_type{ctx_.get_builtin_resolved_type(sema::type_kind::VOID)};
    const auto test_name{test.description
                             .transform([&](ast::string_handle h) {
                                 return ast_module_.ast.get_as<ast::string_expr>(h).value;
                             })
                             .or_else([this] -> stdx::option<std::string> {
                                 return fmt::format("anonymous_test{}", anon_test_counter_++);
                             })};

    auto& fn{gir_module_.add_function(*test_name, void_type, true, false)};
    auto& entry{fn.add_segment()};
    builder_.set_insert_point(fn, entry);

    const scope_guard g{scopes_};
    emit_block(ast_module_.ast.get_as<ast::block_stmt>(test.block));
    if (const auto cur_seg{builder_.get_segment()}) {
        if (!cur_seg->has_terminator()) { builder_.emit_return(); }
    }
}

auto emitter::emit_function(ast::node_id              id,
                            const ast::decl_stmt&     decl,
                            const ast::function_expr& fn_expr) -> void {
    PROFILE_FUNCTION();
    const auto& name_ident{ast_module_.ast.get_as<ast::identifier_expr>(decl.name)};
    const auto  sema_type{ast_module_.get_sema_type_opt(id)};
    ASSERT(sema_type, "Function declaration must have a resolved sema type");

    const auto is_constexpr{decl.has_modifier(ast::decl_modifiers::CONSTEXPR)};
    auto&      fn{gir_module_.add_function(
        std::string{name_ident.name}, *sema_type, false, is_constexpr, fn_expr.variadic)};

    auto& entry{fn.add_segment()};
    builder_.set_insert_point(fn, entry);
    const scope_guard g{scopes_};

    for (const auto& param : fn_expr.parameters) {
        const auto& p_ident{ast_module_.ast.get_as<ast::identifier_expr>(param.name)};
        const auto  p_name{p_ident.name};
        const auto  p_type{ast_module_.get_sema_type_opt(param.name)};
        ASSERT(p_type, "Function parameter must have a resolved sema type");

        auto& p_slot{fn.add_param(std::string{p_name}, *p_type)};
        scopes_.back().bindings.emplace(p_name,
                                        local_binding{p_slot.id, *p_type, false, stdx::none});
    }

    emit_block(ast_module_.ast.get_as<ast::block_stmt>(fn_expr.body));
    if (const auto cur_seg{builder_.get_segment()}) {
        if (!cur_seg->has_terminator()) {
            const auto fn_data{sema_type->get_data().as_opt<sema::types::function>()};
            ASSERT(fn_data, "Function sema type must contain function type data");
            if (fn_data->return_type.get_kind() == sema::type_kind::VOID) {
                builder_.emit_return();
            } else {
                builder_.emit_return(value{undefined_val{}, fn_data->return_type});
            }
        }
    }
}

auto emitter::emit_anonymous_function(ast::node_id id, const ast::function_expr& fn_expr)
    -> std::string {
    PROFILE_FUNCTION();
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    ASSERT(sema_type, "Anonymous function must have a resolved sema type");
    auto&      fn_type{*sema_type};
    const auto anon_name{fmt::format("anonymous_fn{}", anon_fn_counter_++)};

    auto&      fn{gir_module_.add_function(anon_name, fn_type, false, false, fn_expr.variadic)};
    const auto prev_fn{builder_.get_function()};
    const auto prev_seg{builder_.get_segment()};

    auto& entry{fn.add_segment()};
    builder_.set_insert_point(fn, entry);

    {
        const scope_guard g{scopes_};
        for (const auto& param : fn_expr.parameters) {
            const auto& p_ident{ast_module_.ast.get_as<ast::identifier_expr>(param.name)};
            const auto  p_name{p_ident.name};
            const auto  p_type{ast_module_.get_sema_type_opt(param.name)};
            ASSERT(p_type, "Anonymous function parameter must have a resolved sema type");

            auto& p_slot{fn.add_param(std::string{p_name}, *p_type)};
            scopes_.back().bindings.emplace(p_name,
                                            local_binding{p_slot.id, *p_type, false, stdx::none});
        }

        emit_block(ast_module_.ast.get_as<ast::block_stmt>(fn_expr.body));
        if (const auto cur_seg{builder_.get_segment()}) {
            if (!cur_seg->has_terminator()) {
                const auto fn_data{fn_type.get_data().as_opt<sema::types::function>()};
                ASSERT(fn_data, "Function type must contain function type data");
                if (fn_data->return_type.get_kind() == sema::type_kind::VOID) {
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

auto emitter::emit_stmt(const ast::stmt_handle& stmt) -> void {
    PROFILE_FUNCTION();
    const auto stmt_id{*stmt};
    builder_.set_location(ast_module_.ast.location_of(stmt_id));
    ast_module_.ast[stmt_id].visit(
        [&](const auto&) { UNREACHABLE("Unhandled statement node variant in emit_stmt"); },
        [&](const ast::block_stmt& block) { emit_block(block); },
        [&](const ast::decl_stmt& decl) { emit_decl_stmt(stmt_id, decl); },
        [&](const ast::return_stmt& ret) { emit_return_stmt(stmt_id, ret); },
        [&](const ast::defer_stmt& def) { emit_defer_stmt(stmt_id, def); },
        [&](const ast::expr_stmt& expr_st) { emit_expression_id(expr_st.expression); },
        [&](const ast::break_stmt& brk) { emit_break(stmt_id, brk); },
        [&](const ast::continue_stmt& cnt) { emit_continue(stmt_id, cnt); },
        [&](const ast::discard_stmt& discard) { emit_expression(discard.discarded); });
}

auto emitter::emit_stmt_as_value(const ast::stmt_handle& stmt) -> value {
    PROFILE_FUNCTION();
    return ast_module_.ast[stmt].visit(
        [&](const auto&) -> value {
            emit_stmt(stmt);
            return value{void_val{}, ctx_.get_builtin_resolved_type(sema::type_kind::VOID)};
        },
        [&](const ast::expr_stmt& expr_st) -> value {
            return emit_expression_id(expr_st.expression);
        },
        [&](const ast::block_stmt& block) -> value {
            emit_block(block);
            return value{void_val{}, ctx_.get_builtin_resolved_type(sema::type_kind::VOID)};
        });
}

auto emitter::emit_defers_for_scope(usize scope_idx) -> void {
    PROFILE_FUNCTION();
    if (scope_idx >= scopes_.size()) { return; }
    const auto defers{scopes_[scope_idx].defers};
    for (const auto& def_stmt : std::views::reverse(defers)) { emit_stmt(def_stmt); }
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
        const auto& ident{ast_module_.ast.get_as<ast::identifier_expr>(*brk.label)};
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
        const auto& ident{ast_module_.ast.get_as<ast::identifier_expr>(*cnt.label)};
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
    for (const auto& stmt : block.statements) { emit_stmt(stmt); }
    emit_defers_for_scope(scopes_.size() - 1);
}

auto emitter::emit_decl_stmt(ast::node_id id, const ast::decl_stmt& decl) -> void {
    PROFILE_FUNCTION();
    const auto& name_ident{ast_module_.ast.get_as<ast::identifier_expr>(decl.name)};
    const auto  name{name_ident.name};
    const auto  sema_type{ast_module_.get_sema_type_opt(id)};
    ASSERT(sema_type, "Local declaration must have a resolved sema type");

    const auto is_const{decl.has_modifier(ast::decl_modifiers::CONSTANT) ||
                        decl.has_modifier(ast::decl_modifiers::CONSTEXPR)};

    if (is_const && decl.value) {
        if (const auto cv{const_eval_.try_eval(*decl.value)}) {
            scopes_.back().bindings.emplace(
                name,
                local_binding{
                    local_id{0, local_kind::TEMPORARY}, *sema_type, false, cv->to_gir_value()});
            return;
        }

        const auto val{emit_expression(*decl.value)};
        if (const auto lid{val.as_opt<local_id>()}) {
            scopes_.back().bindings.emplace(name,
                                            local_binding{*lid, *sema_type, false, stdx::none});
            return;
        }
        scopes_.back().bindings.emplace(
            name, local_binding{local_id{0, local_kind::TEMPORARY}, *sema_type, false, val});
        return;
    }

    const auto slot{builder_.emit_alloca(*sema_type, name)};
    if (decl.value) {
        const auto val{emit_expression(*decl.value)};
        builder_.emit_store(slot, val);
    }
    scopes_.back().bindings.emplace(name, local_binding{slot, *sema_type, true, stdx::none});
}

auto emitter::emit_return_stmt(ast::node_id, const ast::return_stmt& ret) -> void {
    PROFILE_FUNCTION();
    stdx::option<value> ret_val;
    if (ret.expression) { ret_val.emplace(emit_expression(*ret.expression)); }
    emit_defers_up_to(0);
    builder_.emit_return(ret_val);
}

auto emitter::emit_expression_id(ast::node_id id) -> value {
    PROFILE_FUNCTION();
    ASSERT(id.is_valid(), "Valid node ID expected in emit_expression_id");
    builder_.set_location(ast_module_.ast.location_of(id));

    return ast_module_.ast[id].visit(
        [&](const auto&) -> value {
            UNREACHABLE("Unhandled expression node variant in emit_expression_id");
        },
        [&](ast::i32_expr data) -> value {
            return value{static_cast<i64>(data.value),
                         ctx_.get_builtin_resolved_type(sema::type_kind::I32)};
        },
        [&](ast::i64_expr data) -> value {
            return value{static_cast<i64>(data.value),
                         ctx_.get_builtin_resolved_type(sema::type_kind::I64)};
        },
        [&](ast::isize_expr data) -> value {
            return value{static_cast<i64>(data.value),
                         ctx_.get_builtin_resolved_type(sema::type_kind::ISIZE)};
        },
        [&](ast::u8_expr data) -> value {
            return value{static_cast<u64>(data.value),
                         ctx_.get_builtin_resolved_type(sema::type_kind::U8)};
        },
        [&](ast::u32_expr data) -> value {
            return value{static_cast<u64>(data.value),
                         ctx_.get_builtin_resolved_type(sema::type_kind::U32)};
        },
        [&](ast::u64_expr data) -> value {
            return value{static_cast<u64>(data.value),
                         ctx_.get_builtin_resolved_type(sema::type_kind::U64)};
        },
        [&](ast::usize_expr data) -> value {
            return value{static_cast<u64>(data.value),
                         ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
        },
        [&](ast::f32_expr data) -> value {
            return value{static_cast<f64>(data.value),
                         ctx_.get_builtin_resolved_type(sema::type_kind::F32)};
        },
        [&](ast::f64_expr data) -> value {
            return value{static_cast<f64>(data.value),
                         ctx_.get_builtin_resolved_type(sema::type_kind::F64)};
        },
        [&](ast::bool_expr) -> value {
            return value{id.get_token_type() == syntax::token_type_t::BOOLEAN_TRUE,
                         ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
        },
        [&](const ast::string_expr& data) -> value {
            return value{std::string{data.value}, ast_module_.get_sema_type_opt(id)};
        },
        [&](ast::void_expr) -> value {
            return value{void_val{}, ctx_.get_builtin_resolved_type(sema::type_kind::VOID)};
        },
        [&](ast::undefined_expr) -> value {
            return value{undefined_val{},
                         ctx_.get_builtin_resolved_type(sema::type_kind::UNDEFINED)};
        },
        [&](ast::unreachable_expr) -> value {
            builder_.emit_unreachable();
            return value{undefined_val{},
                         ctx_.get_builtin_resolved_type(sema::type_kind::NORETURN)};
        },
        [&](const ast::identifier_expr& data) -> value { return emit_ident(id, data); },
        [&](const ast::function_expr& data) -> value {
            const auto anon_name{emit_anonymous_function(id, data)};
            return value{anon_name, ast_module_.get_sema_type_opt(id)};
        },
        [&](const ast::if_expr& data) -> value { return emit_if(id, data); },
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
        [&](const ast::assignment_expr& data) -> value { return emit_assignment(id, data); },
        [&](const ast::call_expr& data) -> value { return emit_call(id, data); },
        [&](const ast::array_expr& data) -> value { return emit_array(id, data); },
        [&](ast::grouped_expr) -> value { return emit_expression_id(id); });
}

auto emitter::emit_array(ast::node_id id, const ast::array_expr& arr) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    ASSERT(sema_type, "Array expression must have a resolved sema type");

    if (const auto cv{const_eval_.try_eval(id)}) { return cv->to_gir_value(); }
    const auto array_slot{builder_.emit_alloca(*sema_type)};
    auto&      usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};

    const auto arr_data{sema_type->get_data().as_opt<sema::types::array>()};
    ASSERT(arr_data, "Array sema type must contain array type data");
    auto& elem_type{arr_data->underlying};
    for (u64 i{0}; const auto& item : arr.items) {
        const auto elem_ptr{builder_.emit_get_element_ptr(
            value{array_slot, *sema_type}, {value{i++, usize_type}}, elem_type)};
        const auto val{emit_expression(item)};
        builder_.emit_store(value{elem_ptr, elem_type}, val);
    }

    return value{array_slot, sema_type};
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

    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    return value{undefined_val{}, sema_type};
}

auto emitter::emit_binary(ast::node_id id, const ast::binary_expr& binary) -> value {
    PROFILE_FUNCTION();
    const auto op_type{id.get_token_type()};
    if (op_type == syntax::token_type_t::BOOLEAN_AND) { return emit_logical_and(id, binary); }
    if (op_type == syntax::token_type_t::BOOLEAN_OR) { return emit_logical_or(id, binary); }

    const auto kind_opt{map_binary_op(op_type)};
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    ASSERT(kind_opt.has_value(), "Binary operator must be mapped to instruction kind");
    ASSERT(sema_type.has_value(), "Binary expression must have a resolved sema type");

    const auto lhs{emit_expression(binary.lhs)};
    const auto rhs{emit_expression(binary.rhs)};
    return value{builder_.emit_binary(*kind_opt, lhs, rhs, *sema_type), sema_type};
}

auto emitter::emit_unary(ast::node_id id, const ast::unary_expr& unary) -> value {
    PROFILE_FUNCTION();
    const auto op_type{id.get_token_type()};
    const auto kind_opt{map_unary_op(op_type)};
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    ASSERT(kind_opt.has_value(), "Unary operator must be mapped to instruction kind");
    ASSERT(sema_type.has_value(), "Unary expression must have a resolved sema type");

    const auto operand{emit_expression(unary.rhs)};
    const auto dest{builder_.emit_unary(*kind_opt, operand, *sema_type)};
    return value{dest, sema_type};
}

auto emitter::emit_assignment(ast::node_id id, const ast::assignment_expr& assign) -> value {
    PROFILE_FUNCTION();
    const auto op_type{id.get_token_type()};
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    ASSERT(sema_type.has_value(), "Assignment expression must have a resolved sema type");
    const auto lhs_lval{emit_lvalue(assign.lhs)};
    ASSERT(lhs_lval.type.has_value(), "Assignment LHS must have a resolved type");

    if (op_type == syntax::token_type_t::ASSIGN) {
        const auto rhs{emit_expression(assign.rhs)};
        builder_.emit_store(lhs_lval, rhs);
        return rhs;
    }

    switch (op_type) {
    case syntax::token_type_t::PLUS_ASSIGN:
    case syntax::token_type_t::MINUS_ASSIGN:
    case syntax::token_type_t::STAR_ASSIGN:
    case syntax::token_type_t::SLASH_ASSIGN:
    case syntax::token_type_t::PERCENT_ASSIGN:
    case syntax::token_type_t::BW_AND_ASSIGN:
    case syntax::token_type_t::BW_OR_ASSIGN:
    case syntax::token_type_t::XOR_ASSIGN:
    case syntax::token_type_t::SHL_ASSIGN:
    case syntax::token_type_t::SHR_ASSIGN:     {
        const auto base_kind{map_binary_op(op_type).value_or(instruction_kind::ADD)};
        auto&      target_type{*lhs_lval.type};
        const auto loaded{builder_.emit_load(lhs_lval, target_type)};
        const auto rhs{emit_expression(assign.rhs)};
        const auto res_val{
            value{builder_.emit_binary(base_kind, value{loaded, target_type}, rhs, target_type),
                  target_type}};
        builder_.emit_store(lhs_lval, res_val);
        return res_val;
    }
    default: UNREACHABLE("Unhandled assignment operator type");
    }
}

auto emitter::emit_call(ast::node_id id, const ast::call_expr& call) -> value {
    PROFILE_FUNCTION();
    auto& ret_type{ast_module_.get_sema_type_opt(id).value_or(
        ctx_.get_builtin_resolved_type(sema::type_kind::VOID))};

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
                    auto       cast_kind{instruction_kind::WIDEN_CAST};
                    if (fn_token == syntax::token_type_t::BUILTIN_BIT_CAST) {
                        cast_kind = instruction_kind::BIT_CAST;
                    } else if (fn_token == syntax::token_type_t::BUILTIN_PTR_CAST ||
                               fn_token == syntax::token_type_t::BUILTIN_ALIGN_CAST) {
                        cast_kind = instruction_kind::PTR_CAST;
                    }
                    const auto dest{builder_.emit_cast(cast_kind, operand, ret_type)};
                    return value{dest, ret_type};
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
        case syntax::token_type_t::BUILTIN_SIZE_OF:
        case syntax::token_type_t::BUILTIN_ALIGN_OF:
        case syntax::token_type_t::BUILTIN_TYPE_OF:  {
            if (const auto cv{const_eval_.try_eval(id)}) { return cv->to_gir_value(); }
            break;
        }
        default: {
            if (const auto cv{const_eval_.try_eval(id)}) { return cv->to_gir_value(); }
            break;
        }
        }
    }

    std::string callee_name;
    if (const auto ident{ast_module_.ast.get_as_opt<ast::identifier_expr>(call.function)}) {
        if (const auto binding{lookup_binding(ident->name)}) {
            if (binding->const_val && binding->const_val->template is<std::string>()) {
                callee_name = binding->const_val->template as<std::string>();
            } else {
                callee_name = std::string{ident->name};
            }
        } else {
            callee_name = std::string{ident->name};
        }
    } else if (const auto fn_expr{ast_module_.ast.get_as_opt<ast::function_expr>(call.function)}) {
        callee_name = emit_anonymous_function(*call.function, *fn_expr);
    } else {
        callee_name = fmt::format("anonymous_fn{}", anon_fn_counter_++);
    }

    std::vector<value> args;
    args.reserve(call.arguments.size());
    for (const auto& arg : call.arguments) {
        if (const auto expr_h{arg.as_opt<ast::expr_handle>()}) {
            args.emplace_back(emit_expression(*expr_h));
        }
    }

    // Check if callee targets a generic instantiation
    const auto fn_type_opt{ast_module_.get_sema_type_opt(call.function)};
    for (const auto& req : ast_module_.generic_instantiations) {
        bool fn_match{false};
        if (fn_type_opt && *req.generic_fn_type == *fn_type_opt) {
            fn_match = true;
        } else if (const auto info{ctx_.generic_functions.get_opt(*req.generic_fn_type)};
                   info && info->name && *info->name == callee_name) {
            fn_match = true;
        }

        if (fn_match) {
            VERIFY(req.arg_types.size() == args.size(), "Generic arg types do not match arity");
            bool args_match{true};
            for (const auto& [arg, arg_type] : std::views::zip(args, req.arg_types)) {
                if (arg.type && *arg_type != *arg.type) {
                    args_match = false;
                    break;
                }
            }
            if (args_match) {
                callee_name = req.mangled_name;
                break;
            }
        }
    }

    if (const auto dest{builder_.emit_call(callee_name, std::move(args), ret_type)}) {
        return value{*dest, ret_type};
    }
    return value{void_val{}, ret_type};
}

auto emitter::lookup_binding(std::string_view name) const noexcept
    -> stdx::option<const local_binding&> {
    PROFILE_FUNCTION();
    for (auto& frame : std::views::reverse(scopes_)) {
        if (auto it{frame.bindings.find(name)}; it != frame.bindings.end()) { return it->second; }
    }
    return stdx::none;
}

auto emitter::emit_if(ast::node_id id, const ast::if_expr& if_expr) -> value {
    PROFILE_FUNCTION();
    auto sema_type{ast_module_.get_sema_type_opt(id)};
    if (if_expr.alternate &&
        if_expr.consequence->get_kind() == ast::node_kind::EXPRESSION_STATEMENT) {
        const auto& expr_st{ast_module_.ast.get_as<ast::expr_stmt>(*if_expr.consequence)};
        if (const auto expr_type = ast_module_.get_sema_type_opt(expr_st.expression)) {
            if (expr_type->get_kind() != sema::type_kind::VOID) { sema_type = expr_type; }
        }
    }
    const bool yields_value{if_expr.alternate && sema_type && is_value_type(sema_type->get_kind())};

    // Comptime / constexpr condition evaluation
    if (if_expr.constexpr_condition) {
        const auto cond_cv{const_eval_.try_eval(if_expr.condition)};
        if (!cond_cv) {
            ctx_.diags.emplace_back("Constexpr if condition could not be evaluated at compile time",
                                    sema::error::CONSTEXPR_EVALUATION_FAILED,
                                    ast_module_.ast.location_of(if_expr.condition));
            return value{undefined_val{}, sema_type};
        }

        const auto eval{cond_cv->as_opt<bool>()};
        if (!eval) {
            ctx_.diags.emplace_back("Constexpr if condition must evaluate to a boolean",
                                    sema::error::TYPE_MISMATCH,
                                    ast_module_.ast.location_of(if_expr.condition));
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
    const auto cond_val{emit_expression(if_expr.condition)};

    auto&                  consequence_seg{fn.add_segment()};
    stdx::option<segment&> alternate_seg_ptr;
    if (if_expr.alternate) { alternate_seg_ptr.emplace(fn.add_segment()); }
    auto& merge_seg{fn.add_segment()};

    const auto false_target{alternate_seg_ptr ? alternate_seg_ptr->get_id() : merge_seg.get_id()};
    builder_.emit_cond_goto(cond_val, consequence_seg.get_id(), false_target);

    // Consequence branch
    builder_.set_segment(consequence_seg);
    if (yields_value) {
        builder_.emit_store(*res_slot, emit_stmt_as_value(if_expr.consequence));
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
            builder_.emit_store(*res_slot, emit_stmt_as_value(*if_expr.alternate));
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
                         stdx::option<local_id>         res_slot) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    const bool yields_value{sema_type && sema_type->get_kind() != sema::type_kind::VOID};

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
        const auto cond_val{emit_expression(while_loop.condition)};
        const auto false_target{non_break_seg ? non_break_seg->get_id() : exit_seg.get_id()};
        builder_.emit_cond_goto(cond_val, body_seg.get_id(), false_target);

        // Body segment
        builder_.set_segment(body_seg);
        emit_block(ast_module_.ast.get_as<ast::block_stmt>(while_loop.block));
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
                            stdx::option<local_id>         res_slot) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    const bool yields_value{sema_type && sema_type->get_kind() != sema::type_kind::VOID};

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
        emit_block(ast_module_.ast.get_as<ast::block_stmt>(do_while.block));
        if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
            builder_.emit_goto(cond_seg.get_id());
        }

        // Cond segment
        builder_.set_segment(cond_seg);
        const auto cond_val{emit_expression(do_while.condition)};
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
                                 stdx::option<local_id>         res_slot) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    const bool yields_value{sema_type && sema_type->get_kind() != sema::type_kind::VOID};

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
        emit_block(ast_module_.ast.get_as<ast::block_stmt>(loop.block));
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
                       stdx::option<local_id>         res_slot) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    const bool yields_value{sema_type && sema_type->get_kind() != sema::type_kind::VOID};

    auto fn_opt{builder_.get_function()};
    ASSERT(fn_opt, "For loop must be within an active function");
    auto& fn{*fn_opt};

    if (yields_value && !res_slot) { res_slot.emplace(builder_.emit_alloca(*sema_type)); }
    std::vector<iterable_info> iter_infos;
    iter_infos.reserve(for_loop.iterables.size());

    for (const auto& [iter_handle, capture] :
         std::views::zip(for_loop.iterables, for_loop.captures)) {
        stdx::option<std::string_view> cap_name;
        if (capture.payload.is<ast::identifier_expr>()) {
            cap_name.emplace(ast_module_.ast.get_as<ast::identifier_expr>(capture.payload).name);
        }

        const auto iter_id{*iter_handle};
        if (const auto range{ast_module_.ast.get_as_opt<ast::range_expr>(iter_id)}) {
            const bool inclusive{iter_id.get_token_type() == syntax::token_type_t::DOT_DOT_EQ};

            const auto start_val{emit_expression(range->lhs)};
            const auto end_val{emit_expression(range->rhs)};
            auto*      elem_type{start_val.type ? &*start_val.type
                                                : &ctx_.get_builtin_resolved_type(sema::type_kind::I32)};

            const auto slot{builder_.emit_alloca(*elem_type, cap_name.value_or(""))};
            builder_.emit_store(slot, start_val);

            iter_infos.emplace_back<iterable_info>({
                .is_range     = true,
                .is_inclusive = inclusive,
                .var_slot     = slot,
                .elem_type    = elem_type,
                .end_val      = end_val,
                .capture_name = cap_name,
            });
        } else {
            const auto arr_val{emit_expression(iter_handle)};
            auto&      usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
            const auto idx_slot{builder_.emit_alloca(usize_type)};
            builder_.emit_store(idx_slot, value{static_cast<u64>(0), usize_type});

            stdx::option<sema::type&> elem_type{
                ctx_.get_builtin_resolved_type(sema::type_kind::I32)};
            value end_val{static_cast<u64>(0), usize_type};

            if (arr_val.type) {
                if (const auto arr_data{arr_val.type->get_data().as_opt<sema::types::array>()}) {
                    elem_type.emplace(arr_data->underlying);
                    end_val = value{static_cast<u64>(arr_data->len), usize_type};
                } else if (const auto sl_data{
                               arr_val.type->get_data().as_opt<sema::types::slice>()}) {
                    elem_type.emplace(sl_data->underlying);
                }
            }

            iter_infos.emplace_back<iterable_info>({
                .is_range     = false,
                .is_inclusive = false,
                .var_slot     = idx_slot,
                .elem_type    = elem_type,
                .end_val      = end_val,
                .capture_name = cap_name,
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
            const auto  cur_val{builder_.emit_load(info.var_slot, *info.elem_type)};
            const auto  cmp_kind{info.is_inclusive ? instruction_kind::LE : instruction_kind::LT};
            const auto  cmp_res{builder_.emit_binary(
                cmp_kind, value{cur_val, *info.elem_type}, info.end_val, bool_type)};
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
                if (info.capture_name) {
                    scopes_.back().bindings.emplace(
                        *info.capture_name,
                        local_binding{info.var_slot, *info.elem_type, true, stdx::none});
                }
            }
            emit_block(ast_module_.ast.get_as<ast::block_stmt>(for_loop.block));
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
    const auto  sema_type{ast_module_.get_sema_type_opt(id)};
    const bool  yields_value{sema_type && sema_type->get_kind() != sema::type_kind::VOID};
    const auto& name_ident{ast_module_.ast.get_as<ast::identifier_expr>(label.name)};
    const auto  label_name{name_ident.name};

    stdx::option<local_id> res_slot;
    if (yields_value) { res_slot = builder_.emit_alloca(*sema_type); }

    const auto body_id{*label.body};
    return ast_module_.ast[body_id].visit(
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
            return emit_while(body_id, wl, label_name, res_slot);
        },
        [&](const ast::do_while_loop_expr& dw) -> value {
            return emit_do_while(body_id, dw, label_name, res_slot);
        },
        [&](const ast::infinite_loop_expr& il) -> value {
            return emit_infinite_loop(body_id, il, label_name, res_slot);
        },
        [&](const ast::for_loop_expr& fl) -> value {
            return emit_for(body_id, fl, label_name, res_slot);
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
    const auto lhs_val{emit_expression(binary.lhs)};

    auto fn_opt{builder_.get_function()};
    ASSERT(fn_opt, "Logical AND must be within an active function");
    auto& fn{*fn_opt};

    auto& rhs_seg{fn.add_segment()};
    auto& merge_seg{fn.add_segment()};

    builder_.emit_cond_goto(lhs_val, rhs_seg.get_id(), merge_seg.get_id());

    builder_.set_segment(rhs_seg);
    const auto rhs_val{emit_expression(binary.rhs)};
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

    const auto lhs_val{emit_expression(binary.lhs)};

    auto fn_opt{builder_.get_function()};
    ASSERT(fn_opt, "Logical OR must be within an active function");
    auto& fn{*fn_opt};

    auto& rhs_seg{fn.add_segment()};
    auto& merge_seg{fn.add_segment()};
    builder_.emit_cond_goto(lhs_val, merge_seg.get_id(), rhs_seg.get_id());

    builder_.set_segment(rhs_seg);
    const auto rhs_val{emit_expression(binary.rhs)};
    builder_.emit_store(res_slot, rhs_val);
    if (const auto cur_seg{builder_.get_segment()}; cur_seg && !cur_seg->has_terminator()) {
        builder_.emit_goto(merge_seg.get_id());
    }

    builder_.set_segment(merge_seg);
    const auto loaded{builder_.emit_load(res_slot, bool_type)};
    return value{loaded, bool_type};
}

auto emitter::emit_lvalue(ast::node_id id) -> value {
    PROFILE_FUNCTION();
    ASSERT(id.is_valid(), "Valid node ID expected in emit_lvalue");

    return ast_module_.ast[id].visit(
        [&](const auto&) -> value { return emit_expression_id(id); },
        [&](const ast::identifier_expr& ident) -> value {
            const auto binding{lookup_binding(ident.name)};
            ASSERT(binding, "LValue identifier must be bound in scope");
            return value{binding->id, binding->type};
        },
        [&](const ast::dot_expr& dot) -> value {
            const auto base_lval{emit_lvalue(dot.object)};
            const auto obj_type{ast_module_.get_sema_type_opt(dot.object)};
            ASSERT(obj_type, "Dot expression object must have a resolved type");

            const auto& member_ident{ast_module_.ast.get_as<ast::identifier_expr>(dot.member)};
            const auto& table{ctx_.registry.get(obj_type->get_symbol_table_idx())};
            const auto  proxy{table.get_proxy_opt(member_ident.name)};
            ASSERT(proxy, "Member must exist in struct symbol table");

            const auto [sym, member_idx]{*proxy};
            auto& usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
            auto& field_type{ast_module_.get_sema_type_opt(dot.member).value_or(*obj_type)};

            const auto field_ptr{builder_.emit_get_element_ptr(
                base_lval, {value{static_cast<u64>(member_idx), usize_type}}, field_type)};
            return value{field_ptr, field_type};
        },
        [&](const ast::index_expr& index) -> value {
            const auto base_lval{emit_lvalue(index.array)};
            const auto idx_val{emit_expression(index.index)};
            const auto elem_type_opt{ast_module_.get_sema_type_opt(id)};
            ASSERT(elem_type_opt, "Index expression must have a resolved element type");
            auto& elem_type{*elem_type_opt};

            const auto obj_type{ast_module_.get_sema_type_opt(index.array)};
            ASSERT(obj_type, "Index array operand must have a resolved type");
            if (const auto arr_data{obj_type->get_data().as_opt<sema::types::array>()}) {
                auto& usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
                auto& bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};

                const auto bound_val{value{static_cast<u64>(arr_data->len), usize_type}};
                const auto is_in_bounds{
                    builder_.emit_binary(instruction_kind::LT, idx_val, bound_val, bool_type)};

                auto fn_opt{builder_.get_function()};
                ASSERT(fn_opt, "Bounds check must be within an active function");
                auto& valid_seg{fn_opt->add_segment()};
                auto& oob_seg{fn_opt->add_segment()};

                builder_.emit_cond_goto(
                    value{is_in_bounds, bool_type}, valid_seg.get_id(), oob_seg.get_id());

                builder_.set_segment(oob_seg);
                builder_.emit_unreachable();
                builder_.set_segment(valid_seg);
            }

            const auto elem_ptr{builder_.emit_get_element_ptr(base_lval, {idx_val}, elem_type)};
            return value{elem_ptr, elem_type};
        },
        [&](const ast::dereference_expr& deref) -> value { return emit_expression(deref.rhs); });
}

auto emitter::emit_match(ast::node_id id, const ast::match_expr& match) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    ASSERT(sema_type, "Match expression must have a resolved sema type");
    const bool yields_value{sema_type->get_kind() != sema::type_kind::VOID};

    if (const auto cv{const_eval_.try_eval(id)}) {
        if (const auto i{cv->as_int_opt()}) { return value{*i, sema_type}; }
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

    for (const auto& arm : match.arms) {
        auto& arm_body_seg{fn.add_segment()};
        auto& next_arm_seg{fn.add_segment()};

        const auto pattern_node_id{*arm.pattern};
        const auto is_discard{pattern_node_id.get_token_type() ==
                                  syntax::token_type_t::UNDERSCORE ||
                              ast_module_.ast[pattern_node_id].template is<ast::discarded>()};

        if (is_discard) {
            builder_.emit_goto(arm_body_seg.get_id());
        } else if (const auto range{ast_module_.ast.get_as_opt<ast::range_expr>(pattern_node_id)}) {
            const auto start_val{emit_expression(range->lhs)};
            const auto end_val{emit_expression(range->rhs)};

            const auto ge_cond{
                builder_.emit_binary(instruction_kind::GE, matcher_val, start_val, bool_type)};
            const auto is_inclusive{pattern_node_id.get_token_type() ==
                                    syntax::token_type_t::DOT_DOT_EQ};
            const auto le_kind{is_inclusive ? instruction_kind::LE : instruction_kind::LT};
            const auto le_cond{builder_.emit_binary(le_kind, matcher_val, end_val, bool_type)};
            const auto in_range{builder_.emit_binary(instruction_kind::AND,
                                                     value{ge_cond, bool_type},
                                                     value{le_cond, bool_type},
                                                     bool_type)};
            builder_.emit_cond_goto(
                value{in_range, bool_type}, arm_body_seg.get_id(), next_arm_seg.get_id());
        } else {
            const auto pat_val{emit_expression_id(pattern_node_id)};
            const auto is_eq{
                builder_.emit_binary(instruction_kind::EQ, matcher_val, pat_val, bool_type)};
            builder_.emit_cond_goto(
                value{is_eq, bool_type}, arm_body_seg.get_id(), next_arm_seg.get_id());
        }

        builder_.set_segment(arm_body_seg);
        {
            const scope_guard arm_guard{scopes_};
            if (arm.capture) {
                const auto& cap_ident{ast_module_.ast.get_as<ast::identifier_expr>(*arm.capture)};
                auto&       cap_type{ast_module_.get_sema_type_opt(*arm.capture)
                                   .value_or(ctx_.get_builtin_resolved_type(sema::type_kind::I32))};
                scopes_.back().bindings.emplace(
                    cap_ident.name,
                    local_binding{matcher_val.data.as<local_id>(), cap_type, false, stdx::none});
            }

            if (yields_value && res_slot) {
                const auto arm_val{emit_stmt_as_value(arm.dispatch)};
                builder_.emit_store(*res_slot, arm_val);
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

auto emitter::emit_initializer(ast::node_id id, const ast::initializer_expr& init) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    ASSERT(sema_type, "Initializer expression must have a resolved sema type");

    if (const auto cv{const_eval_.try_eval(id)}) {
        if (const auto i{cv->as_int_opt()}) { return value{*i, sema_type}; }
        if (const auto b{cv->as_opt<bool>()}) { return value{*b, sema_type}; }
        if (const auto f{cv->as_opt<f64>()}) { return value{*f, sema_type}; }
    }

    const auto struct_slot{builder_.emit_alloca(*sema_type)};
    auto&      usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};

    const auto st{sema_type->get_data().as_opt<sema::types::struct_t>()};
    ASSERT(st, "Initializer target must be a struct type");
    const auto& table{ctx_.registry.get(sema_type->get_symbol_table_idx())};
    for (const auto& [accessor, val_expr] : init.initializers) {
        const auto& imp{ast_module_.ast.get_as<ast::implicit_access_expr>(*accessor)};
        const auto& name{ast_module_.ast.get_as<ast::identifier_expr>(imp.member).name};
        const auto  proxy{table.get_proxy_opt(name)};
        ASSERT(proxy, "Member must exist in struct symbol table");
        const auto [sym, field_idx]{*proxy};
        auto&      field_type{ast_module_.get_sema_type_opt(*val_expr).value_or(*sema_type)};
        const auto field_ptr{
            builder_.emit_get_element_ptr(value{struct_slot, *sema_type},
                                          {value{static_cast<u64>(field_idx), usize_type}},
                                          field_type)};
        const auto val{emit_expression(val_expr)};
        builder_.emit_store(value{field_ptr, field_type}, val);
    }

    return value{struct_slot, sema_type};
}

auto emitter::emit_dot(ast::node_id id, const ast::dot_expr& dot) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    const auto obj_type{ast_module_.get_sema_type_opt(dot.object)};
    ASSERT(obj_type, "Dot expression object must have a resolved type");
    const auto member_ident{ast_module_.ast.get_as<ast::identifier_expr>(dot.member)};

    const auto& table{ctx_.registry.get(obj_type->get_symbol_table_idx())};
    if (const auto proxy{table.get_proxy_opt(member_ident.name)}) {
        const auto [sym, member_idx]{*proxy};
        auto& usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};

        if (const auto st{obj_type->get_data().as_opt<sema::types::struct_t>()}) {
            auto&      field_type{sema_type ? *sema_type : *obj_type};
            const auto base_lval{emit_lvalue(dot.object)};
            const auto field_ptr{builder_.emit_get_element_ptr(
                base_lval, {value{static_cast<u64>(member_idx), usize_type}}, field_type)};
            const auto loaded{builder_.emit_load(value{field_ptr, field_type}, field_type)};
            return value{loaded, field_type};
        }
    }

    if (const auto cv{const_eval_.try_eval(id)}) {
        if (const auto i{cv->as_int_opt()}) { return value{*i, sema_type}; }
        if (const auto b{cv->as_opt<bool>()}) { return value{*b, sema_type}; }
        if (const auto f{cv->as_opt<f64>()}) { return value{*f, sema_type}; }
    }

    return value{std::string{member_ident.name}, sema_type};
}

auto emitter::emit_index(ast::node_id id, const ast::index_expr& index) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    ASSERT(sema_type, "Index expression must have a resolved sema type");
    const auto base_lval{emit_lvalue(index.array)};
    const auto idx_val{emit_expression(index.index)};
    auto&      elem_type{*sema_type};

    const auto obj_type{ast_module_.get_sema_type_opt(index.array)};
    ASSERT(obj_type, "Index array operand must have a resolved type");
    if (const auto arr_data{obj_type->get_data().as_opt<sema::types::array>()}) {
        auto& usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
        auto& bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};

        const auto bound_val{value{static_cast<u64>(arr_data->len), usize_type}};
        const auto is_in_bounds{
            builder_.emit_binary(instruction_kind::LT, idx_val, bound_val, bool_type)};

        auto fn_opt{builder_.get_function()};
        ASSERT(fn_opt, "Bounds check must be within an active function");
        auto& valid_seg{fn_opt->add_segment()};
        auto& oob_seg{fn_opt->add_segment()};

        builder_.emit_cond_goto(
            value{is_in_bounds, bool_type}, valid_seg.get_id(), oob_seg.get_id());

        builder_.set_segment(oob_seg);
        builder_.emit_unreachable();
        builder_.set_segment(valid_seg);
    }

    const auto elem_ptr{builder_.emit_get_element_ptr(base_lval, {idx_val}, elem_type)};
    const auto loaded{builder_.emit_load(value{elem_ptr, elem_type}, elem_type)};
    return value{loaded, elem_type};
}

auto emitter::emit_address_of(ast::node_id id, const ast::address_of_expr& addr) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    ASSERT(sema_type, "Address of expression must have a resolved sema type");
    const auto target{emit_lvalue(addr.rhs)};
    auto&      res_type{*sema_type};
    const auto ptr{builder_.emit_address_of(target, res_type)};
    return value{ptr, sema_type};
}

auto emitter::emit_dereference(ast::node_id id, const ast::dereference_expr& deref) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    ASSERT(sema_type, "Dereference expression must have a resolved sema type");
    const auto ptr_val{emit_expression(deref.rhs)};
    auto&      elem_type{*sema_type};
    const auto loaded{builder_.emit_load(ptr_val, elem_type)};
    return value{loaded, sema_type};
}

auto emitter::emit_reference(ast::node_id id, const ast::reference_expr& ref) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    ASSERT(sema_type, "Reference expression must have a resolved sema type");
    const auto target{emit_lvalue(ref.rhs)};
    auto&      res_type{*sema_type};
    const auto ptr{builder_.emit_address_of(target, res_type)};
    return value{ptr, sema_type};
}

auto emitter::emit_implicit_access(ast::node_id id, const ast::implicit_access_expr& imp) -> value {
    PROFILE_FUNCTION();
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    if (const auto cv{const_eval_.try_eval(id)}) {
        if (const auto i{cv->as_int_opt()}) { return value{*i, sema_type}; }
        if (const auto b{cv->as_opt<bool>()}) { return value{*b, sema_type}; }
    }
    const auto& ident{ast_module_.ast.get_as<ast::identifier_expr>(imp.member)};
    return value{std::string{ident.name}, sema_type};
}

auto emitter::emit_module_access(ast::node_id id, const ast::module_access_expr& mod_access)
    -> value {
    PROFILE_FUNCTION();
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    if (const auto cv{const_eval_.try_eval(id)}) {
        if (const auto i{cv->as_int_opt()}) { return value{*i, sema_type}; }
        if (const auto b{cv->as_opt<bool>()}) { return value{*b, sema_type}; }
        if (const auto f{cv->as_opt<f64>()}) { return value{*f, sema_type}; }
    }
    const auto& inner_ident{ast_module_.ast.get_as<ast::identifier_expr>(mod_access.inner)};
    return value{std::string{inner_ident.name}, sema_type};
}

} // namespace ghoti::gir
