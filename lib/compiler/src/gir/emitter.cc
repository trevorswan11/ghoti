#include "compiler/gir/emitter.hh"

#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <stdx/option.hh>
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
#include "compiler/sema/type.hh"
#include "compiler/syntax/token_type.hh"

namespace ghoti::gir {

auto emitter::emit() -> module {
    const_eval_.resolve_all_deferred_types();

    for (const auto root_id : ast_module_.ast) {
        ast_module_.ast[root_id].visit(
            [&](const auto&) {},
            [&](const ast::decl_stmt& decl) { emit_top_level_decl(root_id, decl); },
            [&](const ast::using_stmt& using_stmt) { emit_top_level_using(root_id, using_stmt); },
            [&](const ast::test_stmt& test) { emit_top_level_test(root_id, test); });
    }
    return std::move(gir_module_);
}

auto emitter::emit_top_level_decl(ast::node_id id, const ast::decl_stmt& decl) -> void {
    const auto& name_ident{ast_module_.ast.get_as<ast::identifier_expr>(decl.name)};
    const auto  name{name_ident.name};
    const auto  sema_type{ast_module_.get_sema_type_opt(id)};
    if (!sema_type) { return; }

    if (decl.value) {
        if (const auto fn_expr{ast_module_.ast.get_as_opt<ast::function_expr>(*decl.value)}) {
            return emit_function(id, decl, *fn_expr);
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
    const auto& name_ident{ast_module_.ast.get_as<ast::identifier_expr>(using_stmt.alias)};
    const auto  name{name_ident.name};
    if (const auto sema_type{ast_module_.get_sema_type_opt(using_stmt.explicit_type)}) {
        gir_module_.add_type(std::string{name}, *sema_type);
    }
}

auto emitter::emit_top_level_test(ast::node_id, const ast::test_stmt& test) -> void {
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
    const auto& name_ident{ast_module_.ast.get_as<ast::identifier_expr>(decl.name)};
    const auto  sema_type{ast_module_.get_sema_type_opt(id)};
    if (!sema_type) { return; }

    const auto is_constexpr{decl.has_modifier(ast::decl_modifiers::CONSTEXPR)};
    auto&      fn{
        gir_module_.add_function(std::string{name_ident.name}, *sema_type, false, is_constexpr)};

    auto& entry{fn.add_segment()};
    builder_.set_insert_point(fn, entry);
    const scope_guard g{scopes_};

    for (const auto& param : fn_expr.parameters) {
        const auto& p_ident{ast_module_.ast.get_as<ast::identifier_expr>(param.name)};
        const auto  p_name{p_ident.name};
        const auto  p_type{ast_module_.get_sema_type_opt(param.name)};
        if (!p_type) { continue; }

        auto& p_slot{fn.add_param(std::string{p_name}, *p_type)};
        scopes_.back().bindings.emplace(p_name,
                                        local_binding{p_slot.id, *p_type, false, stdx::none});
    }

    emit_block(ast_module_.ast.get_as<ast::block_stmt>(fn_expr.body));
    if (const auto cur_seg{builder_.get_segment()}) {
        if (!cur_seg->has_terminator()) {
            if (const auto fn_data{sema_type->get_data().as_opt<sema::types::function>()}) {
                if (fn_data->return_type.get_kind() == sema::type_kind::VOID) {
                    builder_.emit_return();
                } else {
                    builder_.emit_return(value{undefined_val{}, fn_data->return_type});
                }
            } else {
                builder_.emit_return();
            }
        }
    }
}

auto emitter::emit_anonymous_function(ast::node_id id, const ast::function_expr& fn_expr)
    -> std::string {
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    auto&      fn_type{sema_type ? *sema_type
                                 : ctx_.get_builtin_resolved_type(sema::type_kind::FUNCTION)};
    const auto anon_name{fmt::format("anonymous_fn{}", anon_fn_counter_++)};

    auto&      fn{gir_module_.add_function(anon_name, fn_type, false, false)};
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
            if (!p_type) { continue; }

            auto& p_slot{fn.add_param(std::string{p_name}, *p_type)};
            scopes_.back().bindings.emplace(p_name,
                                            local_binding{p_slot.id, *p_type, false, stdx::none});
        }

        emit_block(ast_module_.ast.get_as<ast::block_stmt>(fn_expr.body));
        if (const auto cur_seg{builder_.get_segment()}) {
            if (!cur_seg->has_terminator()) {
                if (const auto fn_data{fn_type.get_data().as_opt<sema::types::function>()}) {
                    if (fn_data->return_type.get_kind() == sema::type_kind::VOID) {
                        builder_.emit_return();
                    } else {
                        builder_.emit_return(value{undefined_val{}, fn_data->return_type});
                    }
                } else {
                    builder_.emit_return();
                }
            }
        }
    }

    if (prev_fn && prev_seg) { builder_.set_insert_point(*prev_fn, *prev_seg); }
    return anon_name;
}

auto emitter::emit_stmt(const ast::stmt_handle& stmt) -> void {
    const auto stmt_id{*stmt};
    ast_module_.ast[stmt_id].visit(
        [&](const auto&) {},
        [&](const ast::block_stmt& block) { emit_block(block); },
        [&](const ast::decl_stmt& decl) { emit_decl_stmt(stmt_id, decl); },
        [&](const ast::return_stmt& ret) { emit_return_stmt(stmt_id, ret); },
        [&](const ast::expr_stmt& expr_st) { emit_expression_id(expr_st.expression); },
        [&](const ast::break_stmt& brk) { emit_break(stmt_id, brk); },
        [&](const ast::continue_stmt& cnt) { emit_continue(stmt_id, cnt); },
        [&](const ast::discard_stmt& discard) { emit_expression(discard.discarded); });
}

auto emitter::emit_stmt_as_value(const ast::stmt_handle& stmt) -> value {
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

auto emitter::emit_break(ast::node_id, const ast::break_stmt& brk) -> void {
    if (loop_stack_.empty()) { return; }

    stdx::option<std::string_view> target_label;
    if (brk.label) {
        const auto& ident{ast_module_.ast.get_as<ast::identifier_expr>(*brk.label)};
        target_label.emplace(ident.name);
    }

    for (const auto& [label, break_target, continue_target, result_slot] :
         std::views::reverse(loop_stack_)) {
        if (!target_label || label == *target_label) {
            if (brk.expression && result_slot) {
                builder_.emit_store(*result_slot, emit_expression(*brk.expression));
            }
            builder_.emit_goto(break_target);
            return;
        }
    }
}

auto emitter::emit_continue(ast::node_id, const ast::continue_stmt& cnt) -> void {
    if (loop_stack_.empty()) { return; }

    stdx::option<std::string_view> target_label;
    if (cnt.label) {
        const auto& ident{ast_module_.ast.get_as<ast::identifier_expr>(*cnt.label)};
        target_label.emplace(ident.name);
    }

    for (const auto& [label, break_target, continue_target, result_slot] :
         std::views::reverse(loop_stack_)) {
        if (!target_label || label == *target_label) {
            builder_.emit_goto(continue_target);
            return;
        }
    }
}

auto emitter::emit_block(const ast::block_stmt& block) -> void {
    const scope_guard g{scopes_};
    for (const auto& stmt : block.statements) { emit_stmt(stmt); }
}

auto emitter::emit_decl_stmt(ast::node_id id, const ast::decl_stmt& decl) -> void {
    const auto& name_ident{ast_module_.ast.get_as<ast::identifier_expr>(decl.name)};
    const auto  name{name_ident.name};
    const auto  sema_type{ast_module_.get_sema_type_opt(id)};
    if (!sema_type) { return; }

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
    builder_.emit_return(ret.expression.transform([this](auto h) { return emit_expression(h); }));
}

auto emitter::emit_expression_id(ast::node_id id) -> value {
    if (!id.is_valid()) { return value{undefined_val{}, stdx::none}; }

    return ast_module_.ast[id].visit(
        [&](const auto&) -> value { return value{undefined_val{}, stdx::none}; },
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
        [&](const ast::identifier_expr& data) -> value { return emit_ident(id, data); },
        [&](const ast::function_expr& data) -> value {
            const auto anon_name{emit_anonymous_function(id, data)};
            return value{anon_name, ast_module_.get_sema_type_opt(id)};
        },
        [&](const ast::if_expr& data) -> value { return emit_if(id, data); },
        [&](const ast::while_loop_expr& data) -> value { return emit_while(id, data); },
        [&](const ast::do_while_loop_expr& data) -> value { return emit_do_while(id, data); },
        [&](const ast::infinite_loop_expr& data) -> value { return emit_infinite_loop(id, data); },
        [&](const ast::for_loop_expr& data) -> value { return emit_for(id, data); },
        [&](const ast::label_expr& data) -> value { return emit_label(id, data); },
        [&](const ast::binary_expr& data) -> value { return emit_binary(id, data); },
        [&](const ast::unary_expr& data) -> value { return emit_unary(id, data); },
        [&](const ast::assignment_expr& data) -> value { return emit_assignment(id, data); },
        [&](const ast::call_expr& data) -> value { return emit_call(id, data); },
        [&](ast::grouped_expr) -> value { return emit_expression_id(id); });
}

auto emitter::emit_ident(ast::node_id id, const ast::identifier_expr& ident) -> value {
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
    const auto op_type{id.get_token_type()};
    if (op_type == syntax::token_type_t::BOOLEAN_AND) { return emit_logical_and(id, binary); }
    if (op_type == syntax::token_type_t::BOOLEAN_OR) { return emit_logical_or(id, binary); }

    const auto kind_opt{map_binary_op(op_type)};
    const auto sema_type{ast_module_.get_sema_type_opt(id)};

    const auto lhs{emit_expression(binary.lhs)};
    const auto rhs{emit_expression(binary.rhs)};

    if (kind_opt && sema_type) {
        const auto dest{builder_.emit_binary(*kind_opt, lhs, rhs, *sema_type)};
        return value{dest, sema_type};
    }
    return value{undefined_val{}, sema_type};
}

auto emitter::emit_unary(ast::node_id id, const ast::unary_expr& unary) -> value {
    const auto op_type{id.get_token_type()};
    const auto kind_opt{map_unary_op(op_type)};
    const auto sema_type{ast_module_.get_sema_type_opt(id)};

    const auto operand{emit_expression(unary.rhs)};
    if (kind_opt && sema_type) {
        const auto dest{builder_.emit_unary(*kind_opt, operand, *sema_type)};
        return value{dest, sema_type};
    }
    return value{undefined_val{}, sema_type};
}

auto emitter::emit_assignment(ast::node_id id, const ast::assignment_expr& assign) -> value {
    const auto ident{ast_module_.ast.get_as_opt<ast::identifier_expr>(assign.lhs)};
    if (!ident) { return value{undefined_val{}, stdx::none}; }
    const auto binding{lookup_binding(ident->name)};
    if (!binding) { return value{undefined_val{}, stdx::none}; }

    const auto op_type{id.get_token_type()};
    if (op_type == syntax::token_type_t::ASSIGN) {
        const auto rhs{emit_expression(assign.rhs)};
        builder_.emit_store(binding->id, rhs);
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
        const auto loaded{builder_.emit_load(binding->id, binding->type)};
        const auto rhs{emit_expression(assign.rhs)};
        const auto res_val{
            value{builder_.emit_binary(base_kind, value{loaded, binding->type}, rhs, binding->type),
                  binding->type}};
        builder_.emit_store(binding->id, res_val);
        return res_val;
    }
    default: break;
    }

    return value{undefined_val{}, stdx::none};
}

auto emitter::emit_call(ast::node_id id, const ast::call_expr& call) -> value {
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

    auto& ret_type{ast_module_.get_sema_type_opt(id).value_or(
        ctx_.get_builtin_resolved_type(sema::type_kind::VOID))};
    if (const auto dest{builder_.emit_call(callee_name, std::move(args), ret_type)}) {
        return value{*dest, ret_type};
    }
    return value{void_val{}, ret_type};
}

auto emitter::lookup_binding(std::string_view name) const noexcept
    -> stdx::option<const local_binding&> {
    for (auto& frame : std::views::reverse(scopes_)) {
        if (auto it{frame.bindings.find(name)}; it != frame.bindings.end()) { return it->second; }
    }
    return stdx::none;
}

auto emitter::emit_if(ast::node_id id, const ast::if_expr& if_expr) -> value {
    auto sema_type{ast_module_.get_sema_type_opt(id)};
    if (if_expr.alternate &&
        if_expr.consequence->get_kind() == ast::node_kind::EXPRESSION_STATEMENT) {
        const auto& expr_st{ast_module_.ast.get_as<ast::expr_stmt>(*if_expr.consequence)};
        if (const auto expr_type = ast_module_.get_sema_type_opt(expr_st.expression)) {
            if (expr_type->get_kind() != sema::type_kind::VOID) { sema_type = expr_type; }
        }
    }
    const bool yields_value{sema_type && sema_type->get_kind() != sema::type_kind::VOID};

    // Comptime / constexpr condition evaluation
    if (if_expr.constexpr_condition) {
        if (const auto cond_cv{const_eval_.try_eval(if_expr.condition)}) {
            if (const auto eval{cond_cv->as_opt<bool>()}) {
                if (*eval) {
                    return yields_value
                               ? emit_stmt_as_value(if_expr.consequence)
                               : (emit_stmt(if_expr.consequence), value{void_val{}, sema_type});
                }
                if (if_expr.alternate) {
                    return yields_value
                               ? emit_stmt_as_value(*if_expr.alternate)
                               : (emit_stmt(*if_expr.alternate), value{void_val{}, sema_type});
                }
                return value{void_val{}, sema_type};
            }
        }
    }

    auto fn_opt{builder_.get_function()};
    if (!fn_opt) { return value{undefined_val{}, sema_type}; }
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
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    const bool yields_value{sema_type && sema_type->get_kind() != sema::type_kind::VOID};

    auto fn_opt{builder_.get_function()};
    if (!fn_opt) { return value{undefined_val{}, sema_type}; }
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
                                   {
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
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    const bool yields_value{sema_type && sema_type->get_kind() != sema::type_kind::VOID};

    auto fn_opt{builder_.get_function()};
    if (!fn_opt) { return value{undefined_val{}, sema_type}; }
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
                                   {
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
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    const bool yields_value{sema_type && sema_type->get_kind() != sema::type_kind::VOID};

    auto fn_opt{builder_.get_function()};
    if (!fn_opt) { return value{undefined_val{}, sema_type}; }
    auto& fn{*fn_opt};

    if (yields_value && !res_slot) { res_slot = builder_.emit_alloca(*sema_type); }

    auto& body_seg{fn.add_segment()};
    auto& exit_seg{fn.add_segment()};

    if (const auto cur_seg = builder_.get_segment(); cur_seg && !cur_seg->has_terminator()) {
        builder_.emit_goto(body_seg.get_id());
    }

    {
        const loop_context_guard g{loop_stack_,
                                   {
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
    const auto sema_type{ast_module_.get_sema_type_opt(id)};
    const bool yields_value{sema_type && sema_type->get_kind() != sema::type_kind::VOID};

    auto fn_opt{builder_.get_function()};
    if (!fn_opt) { return value{undefined_val{}, sema_type}; }
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
                                       {
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
            if (!fn_opt) { return value{undefined_val{}, sema_type}; }
            auto& fn{*fn_opt};

            auto& exit_seg{fn.add_segment()};
            {
                const loop_context_guard ctx_g{loop_stack_,
                                               {
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
            if (!fn_opt) { return value{undefined_val{}, sema_type}; }
            auto& fn{*fn_opt};

            auto& exit_seg{fn.add_segment()};
            {
                const loop_context_guard g{loop_stack_,
                                           {
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
    auto&      bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
    const auto res_slot{builder_.emit_alloca(bool_type)};
    builder_.emit_store(res_slot, value{false, bool_type});
    const auto lhs_val{emit_expression(binary.lhs)};

    auto fn_opt{builder_.get_function()};
    if (!fn_opt) { return value{undefined_val{}, bool_type}; }
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
    auto&      bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
    const auto res_slot{builder_.emit_alloca(bool_type)};
    builder_.emit_store(res_slot, value{true, bool_type});

    const auto lhs_val{emit_expression(binary.lhs)};

    auto fn_opt{builder_.get_function()};
    if (!fn_opt) { return value{undefined_val{}, bool_type}; }
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

} // namespace ghoti::gir
