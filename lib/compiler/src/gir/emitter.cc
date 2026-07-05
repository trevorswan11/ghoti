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
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/gir/builder.hh"
#include "compiler/gir/const_eval.hh"
#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
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
                             .value_or(fmt::format("anonymous_test{}", anon_test_counter_++))};

    auto& fn{gir_module_.add_function(test_name, void_type, true, false)};
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

auto emitter::emit_stmt(const ast::stmt_handle& stmt) -> void {
    const auto stmt_id{*stmt};
    ast_module_.ast[stmt_id].visit(
        [&](const auto&) {},
        [&](const ast::block_stmt& block) { emit_block(block); },
        [&](const ast::decl_stmt& decl) { emit_decl_stmt(stmt_id, decl); },
        [&](const ast::return_stmt& ret) { emit_return_stmt(stmt_id, ret); },
        [&](const ast::expr_stmt& expr_st) { emit_expression_id(expr_st.expression); });
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

        if (const auto val{emit_expression(*decl.value)}; val.is<local_id>()) {
            scopes_.back().bindings.emplace(
                name, local_binding{val.as<local_id>(), *sema_type, false, stdx::none});
            return;
        }
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
    const auto ident{ast_module_.ast.get_as_opt<ast::identifier_expr>(call.function)};
    auto       callee_name{ident.transform([](auto i) { return i.name; })
                         .value_or(fmt::format("anonymous_fn{}", anon_fn_counter_++))};

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

} // namespace ghoti::gir
