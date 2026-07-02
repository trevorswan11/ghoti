#include "compiler/gir/const_eval.hh"

#include <algorithm>
#include <bit>
#include <cmath>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/type.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/token_type.hh"
#include "support/counter.hh"

namespace ghoti::gir {

auto const_eval::try_eval(ast::node_id id) -> stdx::option<const_value> {
    const auto key{id.get_index()};
    if (auto cached{memo_cache_.find(key)}; cached != memo_cache_.end()) { return cached->second; }

    auto res{eval_node(id)};
    if (res) { memo_cache_.emplace(key, *res); }
    return res;
}

auto const_eval::eval(ast::node_id id) -> const_value {
    auto res{try_eval(id)};
    if (!res || res->is_poison()) {
        ctx_.diags.emplace_back("Expression cannot be evaluated as a compile-time constant",
                                sema::error::CONSTEXPR_EVALUATION_FAILED,
                                module_.ast.location_of(id));
        return const_value::make_poison();
    }
    return *res;
}

auto const_eval::eval_type_dim(ast::node_id id) -> stdx::option<usize> {
    const auto val{eval(id)};
    if (val.is_poison()) { return stdx::none; }

    if (const auto dim{val.as_opt<u64>()}) { return static_cast<usize>(*dim); }
    if (const auto dim{val.as_opt<i64>()}) {
        if (*dim < 0) {
            ctx_.diags.emplace_back("Array dimension cannot be negative",
                                    sema::error::CONSTEXPR_EVALUATION_FAILED,
                                    module_.ast.location_of(id));
            return stdx::none;
        }
        return static_cast<usize>(*dim);
    }

    ctx_.diags.emplace_back("Array dimension must evaluate to an integer constant",
                            sema::error::CONSTEXPR_EVALUATION_FAILED,
                            module_.ast.location_of(id));
    return stdx::none;
}

auto const_eval::resolve_all_deferred_arrays() -> void {
    for (auto& type_opt : module_.sema_side_tables.explicit_types.values) {
        if (!type_opt) { continue; }
        if (const auto def{type_opt->get_data().as_opt<sema::types::deferred_array>()}) {
            type_opt.emplace(resolve_deferred_array(def->array, def->underlying));
        }
    }

    for (auto& type_opt : module_.sema_side_tables.node_types.values) {
        if (!type_opt) { continue; }
        if (const auto def{type_opt->get_data().as_opt<sema::types::deferred_array>()}) {
            type_opt.emplace(resolve_deferred_array(def->array, def->underlying));
        }
    }
}

auto const_eval::type_align_of(const sema::type& type) -> usize {
    switch (type.get_kind()) {
    case sema::type_kind::U8:
    case sema::type_kind::BOOL:      return 1;
    case sema::type_kind::I32:
    case sema::type_kind::U32:
    case sema::type_kind::F32:       return 4;
    case sema::type_kind::I64:
    case sema::type_kind::U64:
    case sema::type_kind::ISIZE:
    case sema::type_kind::USIZE:
    case sema::type_kind::F64:
    case sema::type_kind::POINTER:
    case sema::type_kind::REFERENCE:
    case sema::type_kind::FUNCTION:  return 8;
    case sema::type_kind::SLICE:     return 8;
    case sema::type_kind::ARRAY:
        if (const auto arr{type.get_data().as_opt<sema::types::array>()}) {
            return type_align_of(arr->underlying);
        }
        if (const auto def{type.get_data().as_opt<sema::types::deferred_array>()}) {
            return type_align_of(def->underlying);
        }
        UNREACHABLE("type_kind::ARRAY associated with improper type");
    case sema::type_kind::STRUCT:
        if (const auto st{type.get_data().as_opt<sema::types::struct_t>()}) {
            return std::ranges::fold_left(
                st->fields | std::views::filter([](const auto* f) { return f != nullptr; }) |
                    std::views::transform([](const auto* f) { return type_align_of(*f); }),
                1UZ,
                [](usize a, usize b) { return std::max(a, b); });
        }
        UNREACHABLE("type_kind::STRUCT associated with improper type");
    case sema::type_kind::UNION:
        if (const auto un{type.get_data().as_opt<sema::types::union_t>()}) {
            return std::ranges::fold_left(
                un->fields | std::views::filter([](const auto* f) { return f != nullptr; }) |
                    std::views::transform([](const auto* f) { return type_align_of(*f); }),
                1UZ,
                [](usize a, usize b) { return std::max(a, b); });
        }
        UNREACHABLE("type_kind::UNION associated with improper type");
    case sema::type_kind::ENUM:
        if (const auto en{type.get_data().as_opt<sema::types::enum_t>()}) {
            return type_align_of(en->underlying);
        }
        UNREACHABLE("type_kind::ENUM associated with improper type");
    default: return 1;
    }
}

auto const_eval::type_size_of(const sema::type& type) -> usize {
    switch (type.get_kind()) {
    case sema::type_kind::VOID:      return 0;
    case sema::type_kind::U8:
    case sema::type_kind::BOOL:      return 1;
    case sema::type_kind::I32:
    case sema::type_kind::U32:
    case sema::type_kind::F32:       return 4;
    case sema::type_kind::I64:
    case sema::type_kind::U64:
    case sema::type_kind::ISIZE:
    case sema::type_kind::USIZE:
    case sema::type_kind::F64:
    case sema::type_kind::POINTER:
    case sema::type_kind::REFERENCE:
    case sema::type_kind::FUNCTION:  return 8;
    case sema::type_kind::SLICE:     return 16;
    case sema::type_kind::ARRAY:
        if (const auto arr{type.get_data().as_opt<sema::types::array>()}) {
            const auto elem_size{type_size_of(arr->underlying)};
            const auto elem_align{type_align_of(arr->underlying)};
            const auto elem_stride{elem_align > 0
                                       ? (elem_size + elem_align - 1) / elem_align * elem_align
                                       : elem_size};
            return arr->len * (elem_stride == 0 ? elem_size : elem_stride);
        }
        UNREACHABLE("type_kind::ARRAY associated with improper type");
    case sema::type_kind::STRUCT:
        if (const auto st{type.get_data().as_opt<sema::types::struct_t>()}) {
            usize current_offset{0};
            usize max_align{1};
            for (const auto* field : st->fields) {
                if (!field) { continue; }
                const auto f_align{type_align_of(*field)};
                max_align = std::max(max_align, f_align);
                if (f_align > 0) {
                    current_offset = (current_offset + f_align - 1) / f_align * f_align;
                }
                current_offset += type_size_of(*field);
            }

            return max_align > 0 ? (current_offset + max_align - 1) / max_align * max_align
                                 : current_offset;
        }
        UNREACHABLE("type_kind::STRUCT associated with improper type");
    case sema::type_kind::UNION:
        if (const auto un{type.get_data().as_opt<sema::types::union_t>()}) {
            usize max_size{0};
            usize max_align{1};
            for (const auto* field : un->fields) {
                if (!field) { continue; }
                max_align = std::max(max_align, type_align_of(*field));
                max_size  = std::max(max_size, type_size_of(*field));
            }
            return max_align > 0 ? (max_size + max_align - 1) / max_align * max_align : max_size;
        }
        UNREACHABLE("type_kind::UNION associated with improper type");
    case sema::type_kind::ENUM:
        if (const auto en{type.get_data().as_opt<sema::types::enum_t>()}) {
            return type_size_of(en->underlying);
        }
        UNREACHABLE("type_kind::ENUM associated with improper type");
    default: return 0;
    }
}

auto const_eval::resolve_deferred_array(const ast::explicit_array_type& array,
                                        sema::type&                     item_type) -> sema::type& {
    if (array.dimension) {
        const auto len{eval_type_dim(*array.dimension).value_or(0)};
        auto&      concrete_array{
            ctx_.get_array(sema::types::mut::CONSTANT, array.null_terminated, len, item_type)};
        return concrete_array;
    }
    return item_type;
}

auto const_eval::lookup_local_binding(std::string_view name) const noexcept
    -> stdx::option<const_value> {
    for (auto& frame : std::views::reverse(call_stack_)) {
        if (auto it{frame.bindings.find(name)}; it != frame.bindings.end()) { return it->second; }
    }
    return stdx::none;
}

auto const_eval::eval_node(ast::node_id id) -> stdx::option<const_value> {
    if (!id.is_valid()) { return stdx::none; }

    return module_.ast[id].visit(
        [](const auto&) -> stdx::option<const_value> { return stdx::none; },
        [&](ast::i32_expr data) {
            return const_value{static_cast<i64>(data.value),
                               ctx_.get_builtin_resolved_type(sema::type_kind::I32)};
        },
        [&](ast::i64_expr data) {
            return const_value{static_cast<i64>(data.value),
                               ctx_.get_builtin_resolved_type(sema::type_kind::I64)};
        },
        [&](ast::isize_expr data) {
            return const_value{static_cast<i64>(data.value),
                               ctx_.get_builtin_resolved_type(sema::type_kind::ISIZE)};
        },
        [&](ast::u8_expr data) {
            return const_value{static_cast<u64>(data.value),
                               ctx_.get_builtin_resolved_type(sema::type_kind::U8)};
        },
        [&](ast::u32_expr data) {
            return const_value{static_cast<u64>(data.value),
                               ctx_.get_builtin_resolved_type(sema::type_kind::U32)};
        },
        [&](ast::u64_expr data) {
            return const_value{static_cast<u64>(data.value),
                               ctx_.get_builtin_resolved_type(sema::type_kind::U64)};
        },
        [&](ast::usize_expr data) {
            return const_value{static_cast<u64>(data.value),
                               ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
        },
        [&](ast::f32_expr data) {
            return const_value{static_cast<f64>(data.value),
                               ctx_.get_builtin_resolved_type(sema::type_kind::F32)};
        },
        [&](ast::f64_expr data) {
            return const_value{static_cast<f64>(data.value),
                               ctx_.get_builtin_resolved_type(sema::type_kind::F64)};
        },
        [&](ast::bool_expr) {
            return const_value{id.get_token_type() == syntax::token_type_t::BOOLEAN_TRUE,
                               ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
        },
        [&](const ast::string_expr& data) { return const_value{std::string{data.value}}; },
        [&](ast::void_expr) {
            return const_value{void_val{}, ctx_.get_builtin_resolved_type(sema::type_kind::VOID)};
        },
        [&](ast::undefined_expr) {
            return const_value{undefined_val{},
                               ctx_.get_builtin_resolved_type(sema::type_kind::UNDEFINED)};
        },
        [&](const ast::binary_expr& data) { return eval_binary(id, data); },
        [&](const ast::unary_expr& data) { return eval_unary(id, data); },
        [&](const ast::identifier_expr& data) { return eval_ident(id, data); },
        [&](const ast::call_expr& data) { return eval_call(id, data); },
        [&](const ast::if_expr& data) { return eval_if(id, data); },
        [&](ast::grouped_expr) { return try_eval(id); });
}

auto const_eval::eval_binary(ast::node_id id, const ast::binary_expr& binary)
    -> stdx::option<const_value> {
    const auto op_type{id.get_token_type()};

    // Short-circuit logical operators
    if (op_type == syntax::token_type_t::BOOLEAN_AND) {
        const auto lhs{try_eval(binary.lhs)};
        if (!lhs || !lhs->is<bool>()) { return stdx::none; }
        if (!lhs->as<bool>()) {
            return const_value{false, ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
        }

        const auto rhs{try_eval(binary.rhs)};
        if (!rhs || !rhs->is<bool>()) { return stdx::none; }
        return const_value{rhs->as<bool>(), ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
    } else if (op_type == syntax::token_type_t::BOOLEAN_OR) {
        const auto lhs{try_eval(binary.lhs)};
        if (!lhs || !lhs->is<bool>()) { return stdx::none; }
        if (lhs->as<bool>()) {
            return const_value{true, ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
        }

        const auto rhs{try_eval(binary.rhs)};
        if (!rhs || !rhs->is<bool>()) { return stdx::none; }
        return const_value{rhs->as<bool>(), ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
    }

    const auto lhs{try_eval(binary.lhs)};
    const auto rhs{try_eval(binary.rhs)};
    if (!lhs || !rhs) { return stdx::none; }
    auto& bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};

    // Float operations
    const auto lhs_is_f64{lhs->is<f64>()};
    const auto rhs_is_f64{rhs->is<f64>()};
    if (lhs_is_f64 || rhs_is_f64) {
        const auto l{lhs_is_f64 ? lhs->as<f64>()
                                : (lhs->is<i64>() ? static_cast<f64>(lhs->as<i64>())
                                                  : static_cast<f64>(lhs->as<u64>()))};
        const auto r{rhs_is_f64 ? rhs->as<f64>()
                                : (rhs->is<i64>() ? static_cast<f64>(rhs->as<i64>())
                                                  : static_cast<f64>(rhs->as<u64>()))};
        const auto res_type{lhs->get_type() ? lhs->get_type() : rhs->get_type()};

        switch (op_type) {
        case syntax::token_type_t::PLUS:  return const_value{l + r, res_type};
        case syntax::token_type_t::MINUS: return const_value{l - r, res_type};
        case syntax::token_type_t::STAR:  return const_value{l * r, res_type};
        case syntax::token_type_t::SLASH:
            if (r == 0.0) {
                ctx_.diags.emplace_back("Division by zero in compile-time constant expression",
                                        sema::error::CONSTEXPR_EVALUATION_FAILED,
                                        module_.ast.location_of(id));
                return const_value::make_poison();
            }
            return const_value{l / r, res_type};
        case syntax::token_type_t::EQ:    return const_value{l == r, bool_type};
        case syntax::token_type_t::NEQ:   return const_value{l != r, bool_type};
        case syntax::token_type_t::LT:    return const_value{l < r, bool_type};
        case syntax::token_type_t::LT_EQ: return const_value{l <= r, bool_type};
        case syntax::token_type_t::GT:    return const_value{l > r, bool_type};
        case syntax::token_type_t::GT_EQ: return const_value{l >= r, bool_type};
        default:                          return stdx::none;
        }
    }

    // Unsigned integer operations
    const auto is_unsigned{(lhs->is<u64>() || rhs->is<u64>()) && !lhs->is<i64>()};
    if (is_unsigned) {
        const auto l{lhs->is<u64>() ? lhs->as<u64>() : static_cast<u64>(lhs->as<i64>())};
        const auto r{rhs->is<u64>() ? rhs->as<u64>() : static_cast<u64>(rhs->as<i64>())};
        const auto res_type{lhs->get_type() ? lhs->get_type() : rhs->get_type()};

        switch (op_type) {
        case syntax::token_type_t::PLUS:  return const_value{l + r, res_type};
        case syntax::token_type_t::MINUS: return const_value{l - r, res_type};
        case syntax::token_type_t::STAR:  return const_value{l * r, res_type};
        case syntax::token_type_t::SLASH:
            if (r == 0) {
                ctx_.diags.emplace_back("Division by zero in compile-time constant expression",
                                        sema::error::CONSTEXPR_EVALUATION_FAILED,
                                        module_.ast.location_of(id));
                return const_value::make_poison();
            }
            return const_value{l / r, res_type};
        case syntax::token_type_t::PERCENT:
            if (r == 0) {
                ctx_.diags.emplace_back("Modulo by zero in compile-time constant expression",
                                        sema::error::CONSTEXPR_EVALUATION_FAILED,
                                        module_.ast.location_of(id));
                return const_value::make_poison();
            }
            return const_value{l % r, res_type};
        case syntax::token_type_t::BW_AND: return const_value{l & r, res_type};
        case syntax::token_type_t::BW_OR:  return const_value{l | r, res_type};
        case syntax::token_type_t::CARET:  return const_value{l ^ r, res_type};
        case syntax::token_type_t::SHL:    return const_value{l << r, res_type};
        case syntax::token_type_t::SHR:    return const_value{l >> r, res_type};
        case syntax::token_type_t::EQ:     return const_value{l == r, bool_type};
        case syntax::token_type_t::NEQ:    return const_value{l != r, bool_type};
        case syntax::token_type_t::LT:     return const_value{l < r, bool_type};
        case syntax::token_type_t::LT_EQ:  return const_value{l <= r, bool_type};
        case syntax::token_type_t::GT:     return const_value{l > r, bool_type};
        case syntax::token_type_t::GT_EQ:  return const_value{l >= r, bool_type};
        default:                           return stdx::none;
        }
    }

    // Signed integer operations
    const auto is_signed{lhs->is<i64>() || rhs->is<i64>()};
    if (is_signed && (lhs->is<i64>() || lhs->is<u64>()) && (rhs->is<i64>() || rhs->is<u64>())) {
        const auto l{lhs->is<i64>() ? lhs->as<i64>() : static_cast<i64>(lhs->as<u64>())};
        const auto r{rhs->is<i64>() ? rhs->as<i64>() : static_cast<i64>(rhs->as<u64>())};
        const auto res_type{lhs->get_type() ? lhs->get_type() : rhs->get_type()};

        switch (op_type) {
        case syntax::token_type_t::PLUS:  return const_value{l + r, res_type};
        case syntax::token_type_t::MINUS: return const_value{l - r, res_type};
        case syntax::token_type_t::STAR:  return const_value{l * r, res_type};
        case syntax::token_type_t::SLASH:
            if (r == 0) {
                ctx_.diags.emplace_back("Division by zero in compile-time constant expression",
                                        sema::error::CONSTEXPR_EVALUATION_FAILED,
                                        module_.ast.location_of(id));
                return const_value::make_poison();
            }
            return const_value{l / r, res_type};
        case syntax::token_type_t::PERCENT:
            if (r == 0) {
                ctx_.diags.emplace_back("Modulo by zero in compile-time constant expression",
                                        sema::error::CONSTEXPR_EVALUATION_FAILED,
                                        module_.ast.location_of(id));
                return const_value::make_poison();
            }
            return const_value{l % r, res_type};
        case syntax::token_type_t::BW_AND: return const_value{l & r, res_type};
        case syntax::token_type_t::BW_OR:  return const_value{l | r, res_type};
        case syntax::token_type_t::CARET:  return const_value{l ^ r, res_type};
        case syntax::token_type_t::SHL:    return const_value{l << r, res_type};
        case syntax::token_type_t::SHR:    return const_value{l >> r, res_type};
        case syntax::token_type_t::EQ:     return const_value{l == r, bool_type};
        case syntax::token_type_t::NEQ:    return const_value{l != r, bool_type};
        case syntax::token_type_t::LT:     return const_value{l < r, bool_type};
        case syntax::token_type_t::LT_EQ:  return const_value{l <= r, bool_type};
        case syntax::token_type_t::GT:     return const_value{l > r, bool_type};
        case syntax::token_type_t::GT_EQ:  return const_value{l >= r, bool_type};
        default:                           return stdx::none;
        }
    }

    if (lhs->is<bool>() && rhs->is<bool>()) {
        const auto l{lhs->as<bool>()};
        const auto r{rhs->as<bool>()};
        switch (op_type) {
        case syntax::token_type_t::EQ:  return const_value{l == r, bool_type};
        case syntax::token_type_t::NEQ: return const_value{l != r, bool_type};
        default:                        return stdx::none;
        }
    }

    return stdx::none;
}

auto const_eval::eval_unary(ast::node_id id, const ast::unary_expr& unary)
    -> stdx::option<const_value> {
    const auto val{try_eval(unary.rhs)};
    if (!val) { return stdx::none; }

    const auto op_type{id.get_token_type()};
    if (op_type == syntax::token_type_t::MINUS) {
        if (val->is<i64>()) { return const_value{-val->as<i64>(), val->get_type()}; }
        if (val->is<u64>()) {
            return const_value{-static_cast<i64>(val->as<u64>()), val->get_type()};
        }
        if (val->is<f64>()) { return const_value{-val->as<f64>(), val->get_type()}; }
    } else if (op_type == syntax::token_type_t::BANG) {
        if (val->is<bool>()) { return const_value{!val->as<bool>(), val->get_type()}; }
    } else if (op_type == syntax::token_type_t::NOT) {
        if (val->is<i64>()) { return const_value{~val->as<i64>(), val->get_type()}; }
        if (val->is<u64>()) { return const_value{~val->as<u64>(), val->get_type()}; }
    }

    return stdx::none;
}

auto const_eval::eval_ident(ast::node_id, const ast::identifier_expr& ident)
    -> stdx::option<const_value> {
    // Check local call frames
    if (auto local_val{lookup_local_binding(ident.name)}) { return local_val; }

    // Check symbol tables
    if (!module_.root_table_idx) { return stdx::none; }
    const auto& table{ctx_.registry.get(*module_.root_table_idx)};
    const auto  sym_opt{table.get_opt(ident.name)};
    if (!sym_opt) { return stdx::none; }
    const auto& sym{*sym_opt};

    if (const auto node{sym.get_data().as_opt<sema::symbols::node_t>()}) {
        if (const auto decl{module_.ast.get_as_opt<ast::decl_stmt>(*node)}) {
            if (decl->has_modifier(ast::decl_modifiers::CONSTEXPR) ||
                decl->has_modifier(ast::decl_modifiers::CONSTANT)) {
                if (decl->value) { return try_eval(*decl->value); }
            }
        }
    }

    return stdx::none;
}

auto const_eval::eval_call(ast::node_id id, const ast::call_expr& call)
    -> stdx::option<const_value> {
    // Check if callee is a builtin function
    const auto fn_token{call.function->get_token_type()};
    if (syntax::get_builtin_opt(fn_token)) { return eval_builtin(call, fn_token); }

    // Check if callee is a constexpr function
    if (const auto ident{module_.ast.get_as_opt<ast::identifier_expr>(call.function)}) {
        if (!module_.root_table_idx) { return stdx::none; }
        const auto& table{ctx_.registry.get(*module_.root_table_idx)};

        const auto sym{table.get_opt(ident->name)};
        if (!sym) { return stdx::none; }
        const auto node{sym->get_data().as_opt<sema::symbols::node_t>()};
        if (!node) { return stdx::none; }
        const auto decl{module_.ast.get_as_opt<ast::decl_stmt>(*node)};
        if (!decl) { return stdx::none; }

        if (decl->has_modifier(ast::decl_modifiers::CONSTEXPR) && decl->value) {
            if (const auto fn_expr{module_.ast.get_as_opt<ast::function_expr>(*decl->value)}) {
                std::vector<const_value> args;
                for (const auto& arg : call.arguments) {
                    if (const auto expr_h{arg.as_opt<ast::expr_handle>()}) {
                        const auto arg_val{try_eval(*expr_h)};
                        if (!arg_val) { return stdx::none; }
                        args.emplace_back(*arg_val);
                    }
                }
                return eval_constexpr_fn(id, *fn_expr, args);
            }
        }
    }

    return stdx::none;
}

auto const_eval::eval_builtin(const ast::call_expr& call, syntax::token_type_t builtin_type)
    -> stdx::option<const_value> {
    auto& usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};

    switch (builtin_type) {
    case syntax::token_type_t::BUILTIN_SIZE_OF: {
        if (call.arguments.empty()) { return stdx::none; }
        const auto&               arg{call.arguments.front()};
        stdx::option<sema::type&> target_type;
        if (const auto type_id{arg.as_opt<ast::explicit_type_id>()}) {
            target_type = module_.get_sema_type_opt(*type_id);
        } else if (const auto expr_h{arg.as_opt<ast::expr_handle>()}) {
            target_type = module_.get_sema_type_opt(*expr_h);
        }
        if (!target_type) { return stdx::none; }

        if (const auto def{target_type->get_data().as_opt<sema::types::deferred_array>()}) {
            if (def->array.dimension) {
                const auto dim_opt{eval_type_dim(*def->array.dimension)};
                const auto len{dim_opt.value_or(0)};
                const auto elem_size{type_size_of(def->underlying)};
                const auto elem_align{type_align_of(def->underlying)};
                const auto elem_stride{elem_align > 0
                                           ? (elem_size + elem_align - 1) / elem_align * elem_align
                                           : elem_size};
                const auto total_sz{len * (elem_stride == 0 ? elem_size : elem_stride)};
                return const_value{static_cast<u64>(total_sz), usize_type};
            }
        }
        const auto sz{type_size_of(*target_type)};
        return const_value{static_cast<u64>(sz), usize_type};
    }
    case syntax::token_type_t::BUILTIN_ALIGN_OF: {
        if (call.arguments.empty()) { return stdx::none; }
        const auto&               arg{call.arguments.front()};
        stdx::option<sema::type&> target_type;
        if (const auto type_id = arg.as_opt<ast::explicit_type_id>()) {
            target_type = module_.get_sema_type_opt(*type_id);
        } else if (const auto expr_h = arg.as_opt<ast::expr_handle>()) {
            target_type = module_.get_sema_type_opt(*expr_h);
        }
        if (!target_type) { return stdx::none; }

        if (const auto def{target_type->get_data().as_opt<sema::types::deferred_array>()}) {
            return const_value{static_cast<u64>(type_align_of(def->underlying)), usize_type};
        }
        const auto al{type_align_of(*target_type)};
        return const_value{static_cast<u64>(al), usize_type};
    }
    case syntax::token_type_t::BUILTIN_TYPE_OF: {
        if (call.arguments.empty()) { return stdx::none; }
        const auto& arg{call.arguments.front()};
        if (const auto expr_h{arg.as_opt<ast::expr_handle>()}) {
            const auto target_type{module_.get_sema_type_opt(*expr_h)};
            if (target_type) { return const_value{target_type}; }
        }
        return stdx::none;
    }
    case syntax::token_type_t::BUILTIN_ABS: {
        if (call.arguments.empty()) { return stdx::none; }
        const auto expr_h{call.arguments.front().as_opt<ast::expr_handle>()};
        if (!expr_h) { return stdx::none; }
        const auto arg{try_eval(*expr_h)};
        if (!arg) { return stdx::none; }
        if (arg->is<i64>()) {
            const auto v{arg->as<i64>()};
            return const_value{v < 0 ? -v : v, arg->get_type()};
        }
        if (arg->is<f64>()) { return const_value{std::abs(arg->as<f64>()), arg->get_type()}; }
        return arg;
    }
    case syntax::token_type_t::BUILTIN_SQRT: {
        if (call.arguments.empty()) { return stdx::none; }
        const auto expr_h{call.arguments.front().as_opt<ast::expr_handle>()};
        if (!expr_h) { return stdx::none; }
        const auto arg{try_eval(*expr_h)};
        if (!arg || !arg->is<f64>()) { return stdx::none; }
        return const_value{std::sqrt(arg->as<f64>()), arg->get_type()};
    }
    case syntax::token_type_t::BUILTIN_FLOOR: {
        if (call.arguments.empty()) { return stdx::none; }
        const auto expr_h{call.arguments.front().as_opt<ast::expr_handle>()};
        if (!expr_h) { return stdx::none; }
        const auto arg{try_eval(*expr_h)};
        if (!arg || !arg->is<f64>()) { return stdx::none; }
        return const_value{std::floor(arg->as<f64>()), arg->get_type()};
    }
    case syntax::token_type_t::BUILTIN_CEIL: {
        if (call.arguments.empty()) { return stdx::none; }
        const auto expr_h{call.arguments.front().as_opt<ast::expr_handle>()};
        if (!expr_h) { return stdx::none; }
        const auto arg{try_eval(*expr_h)};
        if (!arg || !arg->is<f64>()) { return stdx::none; }
        return const_value{std::ceil(arg->as<f64>()), arg->get_type()};
    }
    case syntax::token_type_t::BUILTIN_CLZ: {
        if (call.arguments.empty()) { return stdx::none; }
        const auto expr_h{call.arguments.front().as_opt<ast::expr_handle>()};
        if (!expr_h) { return stdx::none; }
        const auto arg{try_eval(*expr_h)};
        if (!arg) { return stdx::none; }
        if (!arg->is<u64>() && !arg->is<i64>()) { return stdx::none; }
        const auto v{arg->is<u64>() ? arg->as<u64>() : static_cast<u64>(arg->as<i64>())};
        return const_value{static_cast<u64>(std::countl_zero(v)), usize_type};
    }
    case syntax::token_type_t::BUILTIN_CTZ: {
        if (call.arguments.empty()) { return stdx::none; }
        const auto expr_h{call.arguments.front().as_opt<ast::expr_handle>()};
        if (!expr_h) { return stdx::none; }
        const auto arg{try_eval(*expr_h)};
        if (!arg) { return stdx::none; }
        if (!arg->is<u64>() && !arg->is<i64>()) { return stdx::none; }
        const auto v{arg->is<u64>() ? arg->as<u64>() : static_cast<u64>(arg->as<i64>())};
        return const_value{static_cast<u64>(std::countr_zero(v)), usize_type};
    }
    case syntax::token_type_t::BUILTIN_POP_COUNT: {
        if (call.arguments.empty()) { return stdx::none; }
        const auto expr_h{call.arguments.front().as_opt<ast::expr_handle>()};
        if (!expr_h) { return stdx::none; }
        const auto arg{try_eval(*expr_h)};
        if (!arg) { return stdx::none; }
        if (!arg->is<u64>() && !arg->is<i64>()) { return stdx::none; }
        const auto v{arg->is<u64>() ? arg->as<u64>() : static_cast<u64>(arg->as<i64>())};
        return const_value{static_cast<u64>(std::popcount(v)), usize_type};
    }
    default: return stdx::none;
    }
}

auto const_eval::eval_constexpr_fn(ast::node_id                    call_id,
                                   const ast::function_expr&       fn_expr,
                                   const std::vector<const_value>& args)
    -> stdx::option<const_value> {
    if (recursion_depth_ >= max_recursion_depth_) {
        ctx_.diags.emplace_back("Constexpr function recursion limit exceeded",
                                sema::error::CONSTEXPR_RECURSION_LIMIT_EXCEEDED,
                                module_.ast.location_of(call_id));
        return const_value::make_poison();
    }

    call_frame frame;
    for (const auto& [param, arg] : std::views::zip(fn_expr.parameters, args)) {
        const auto& ident{module_.ast.get_as<ast::identifier_expr>(param.name)};
        frame.bindings.emplace(ident.name, arg);
    }

    call_stack_.emplace_back(std::move(frame));
    const default_counter::guard g{recursion_depth_};
    const auto                   block_res{eval_stmt(fn_expr.body)};
    call_stack_.pop_back();
    return block_res;
}

auto const_eval::eval_stmt(const ast::stmt_handle& stmt) -> stdx::option<const_value> {
    return module_.ast[*stmt].visit(
        [&](const auto&) -> stdx::option<const_value> { return stdx::none; },
        [&](const ast::block_stmt& data) { return eval_block(*stmt, data); },
        [&](const ast::decl_stmt& data) { return eval_decl(*stmt, data); },
        [&](const ast::return_stmt& data) -> stdx::option<const_value> {
            if (data.expression) { return try_eval(*data.expression); }
            return const_value{void_val{}, ctx_.get_builtin_resolved_type(sema::type_kind::VOID)};
        },
        [&](const ast::expr_stmt& data) -> stdx::option<const_value> {
            if (module_.ast[data.expression].template is<ast::if_expr>()) {
                return try_eval(data.expression);
            }
            return stdx::none;
        });
}

auto const_eval::eval_block(ast::node_id, const ast::block_stmt& block)
    -> stdx::option<const_value> {
    for (const auto& stmt : block.statements) {
        if (const auto res{eval_stmt(stmt)}) { return res; }
    }
    return stdx::none;
}

auto const_eval::eval_decl(ast::node_id, const ast::decl_stmt& decl) -> stdx::option<const_value> {
    if (decl.value && !call_stack_.empty()) {
        if (const auto val{try_eval(*decl.value)}) {
            const auto& ident{module_.ast.get_as<ast::identifier_expr>(decl.name)};
            call_stack_.back().bindings.emplace(ident.name, *val);
        }
    }
    return stdx::none;
}

auto const_eval::eval_if(ast::node_id, const ast::if_expr& if_expr) -> stdx::option<const_value> {
    const auto cond{try_eval(if_expr.condition)};
    if (!cond || !cond->is<bool>()) { return stdx::none; }

    if (cond->as<bool>()) {
        return eval_stmt(if_expr.consequence);
    } else if (if_expr.alternate) {
        return eval_stmt(*if_expr.alternate);
    }
    return stdx::none;
}

} // namespace ghoti::gir
