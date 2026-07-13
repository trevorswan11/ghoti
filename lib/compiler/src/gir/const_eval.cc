#include "compiler/gir/const_eval.hh"

#include <algorithm>
#include <bit>
#include <cmath>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
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

namespace {

template <typename T>
[[nodiscard]] auto fold_binary_arithmetic(syntax::token_type_t      op_type,
                                          T                         l,
                                          T                         r,
                                          stdx::option<sema::type&> res_type,
                                          sema::type&               bool_type,
                                          auto&& on_div_zero) -> stdx::option<const_value> {
    switch (op_type) {
    case syntax::token_type_t::PLUS:  return const_value{l + r, res_type};
    case syntax::token_type_t::MINUS: return const_value{l - r, res_type};
    case syntax::token_type_t::STAR:  return const_value{l * r, res_type};
    case syntax::token_type_t::SLASH:
        if (r == 0) { return on_div_zero("Division by zero in compile-time constant expression"); }
        return const_value{l / r, res_type};
    case syntax::token_type_t::PERCENT:
        if constexpr (std::is_integral_v<T>) {
            if (r == 0) {
                return on_div_zero("Modulo by zero in compile-time constant expression");
            }
            return const_value{l % r, res_type};
        } else {
            return stdx::none;
        }
    case syntax::token_type_t::BW_AND:
        if constexpr (std::is_integral_v<T>) { return const_value{l & r, res_type}; }
        return stdx::none;
    case syntax::token_type_t::BW_OR:
        if constexpr (std::is_integral_v<T>) { return const_value{l | r, res_type}; }
        return stdx::none;
    case syntax::token_type_t::CARET:
        if constexpr (std::is_integral_v<T>) { return const_value{l ^ r, res_type}; }
        return stdx::none;
    case syntax::token_type_t::SHL:
        if constexpr (std::is_integral_v<T>) { return const_value{l << r, res_type}; }
        return stdx::none;
    case syntax::token_type_t::SHR:
        if constexpr (std::is_integral_v<T>) { return const_value{l >> r, res_type}; }
        return stdx::none;
    case syntax::token_type_t::EQ:    return const_value{l == r, bool_type};
    case syntax::token_type_t::NEQ:   return const_value{l != r, bool_type};
    case syntax::token_type_t::LT:    return const_value{l < r, bool_type};
    case syntax::token_type_t::LT_EQ: return const_value{l <= r, bool_type};
    case syntax::token_type_t::GT:    return const_value{l > r, bool_type};
    case syntax::token_type_t::GT_EQ: return const_value{l >= r, bool_type};
    default:                          return stdx::none;
    }
}

} // namespace

auto const_eval::try_eval(ast::node_id id) -> stdx::option<const_value> {
    const auto key{id.get_index()};
    if (call_stack_.empty()) {
        if (auto cached{memo_cache_.find(key)}; cached != memo_cache_.end()) {
            return cached->second;
        }
    }

    auto res{eval_node(id)};
    if (res && call_stack_.empty()) { memo_cache_.emplace(key, *res); }
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

auto const_eval::resolve_all_deferred_calls() -> void {
    for (auto& type_opt : module_.sema_side_tables.explicit_types.values) {
        if (!type_opt) { continue; }
        if (const auto def{type_opt->get_data().as_opt<sema::types::deferred_call>()}) {
            type_opt.emplace(resolve_deferred_call(def->call));
        }
    }

    for (auto& type_opt : module_.sema_side_tables.node_types.values) {
        if (!type_opt) { continue; }
        if (const auto def{type_opt->get_data().as_opt<sema::types::deferred_call>()}) {
            type_opt.emplace(resolve_deferred_call(def->call));
        }
    }
}

auto const_eval::resolve_all_deferred_arrays() -> void {
    resolve_all_deferred_calls();

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

auto const_eval::resolve_all_deferred_types() -> void { resolve_all_deferred_arrays(); }

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
    ASSERT(array.dimension.has_value(), "Deferred array type must have a dimension");
    const auto len{eval_type_dim(*array.dimension).value_or(0)};
    auto&      concrete_array{
        ctx_.get_array(sema::types::mut::CONSTANT, array.null_terminated, len, item_type)};
    return concrete_array;
}

auto const_eval::resolve_deferred_call(const ast::call_expr& call) -> sema::type& {
    const auto val{eval_call(ast::node_id::make_invalid(), call)};
    if (val) {
        if (const auto type_opt{val->as_opt<stdx::option<sema::type&>>()}) {
            if (*type_opt) { return **type_opt; }
        }
        if (const auto sema_type{val->get_type()}) {
            if (sema_type->get_kind() == sema::type_kind::TYPE) {
                if (const auto meta{sema_type->get_data().as_opt<sema::types::meta_type>()}) {
                    return meta->instance;
                }
            }
            return *sema_type;
        }
    }
    ctx_.diags.emplace_back("Failed to evaluate compile-time type constructor function",
                            sema::error::CONSTEXPR_EVALUATION_FAILED,
                            module_.ast.location_of(call.function));
    return ctx_.get_poison();
}

auto const_eval::lookup_local_binding(std::string_view name) const noexcept
    -> stdx::option<const_value> {
    for (auto& frame : std::views::reverse(call_stack_)) {
        if (auto it{frame.bindings.find(name)}; it != frame.bindings.end()) { return it->second; }
    }
    return stdx::none;
}

auto const_eval::mutate_local_binding(std::string_view name, const_value val) -> bool {
    for (auto& frame : std::views::reverse(call_stack_)) {
        if (auto it{frame.bindings.find(name)}; it != frame.bindings.end()) {
            it->second = std::move(val);
            return true;
        }
    }
    return false;
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
        [&](ast::unreachable_expr) {
            return const_value{undefined_val{},
                               ctx_.get_builtin_resolved_type(sema::type_kind::NORETURN)};
        },
        [&](const ast::array_expr& data) { return eval_array(id, data); },
        [&](const ast::index_expr& data) { return eval_index(id, data); },
        [&](const ast::initializer_expr& data) { return eval_initializer(id, data); },
        [&](const ast::dot_expr& data) { return eval_dot(id, data); },
        [&](const ast::implicit_access_expr& data) { return eval_implicit_access(id, data); },
        [&](const ast::module_access_expr& data) { return eval_module_access(id, data); },
        [&](const ast::match_expr& data) { return eval_match(id, data); },
        [&](const ast::assignment_expr& data) {
            return eval_assignment(id, data, id.get_token_type());
        },
        [&](const ast::binary_expr& data) { return eval_binary(id, data); },
        [&](const ast::unary_expr& data) { return eval_unary(id, data); },
        [&](const ast::identifier_expr& data) { return eval_ident(id, data); },
        [&](const ast::call_expr& data) { return eval_call(id, data); },
        [&](const ast::if_expr& data) { return eval_if(id, data); },
        [&](const ast::while_loop_expr& data) { return eval_while(id, data); },
        [&](const ast::do_while_loop_expr& data) { return eval_do_while(id, data); },
        [&](const ast::for_loop_expr& data) { return eval_for(id, data); },
        [&](const ast::struct_expr&) { return const_value{module_.get_sema_type_opt(id)}; },
        [&](const ast::enum_expr&) { return const_value{module_.get_sema_type_opt(id)}; },
        [&](const ast::union_expr&) { return const_value{module_.get_sema_type_opt(id)}; },
        [&](ast::grouped_expr) { return try_eval(id); });
}

auto const_eval::eval_array(ast::node_id id, const ast::array_expr& array)
    -> stdx::option<const_value> {
    const_array arr;
    arr.elements.reserve(array.items.size());
    for (const auto& item_h : array.items) {
        const auto item_val{try_eval(item_h)};
        if (!item_val) { return stdx::none; }
        arr.elements.emplace_back(*item_val);
    }
    const auto sema_type{module_.get_sema_type_opt(id)};
    return const_value{std::move(arr), sema_type};
}

auto const_eval::eval_index(ast::node_id id, const ast::index_expr& index_expr)
    -> stdx::option<const_value> {
    const auto target_val{try_eval(index_expr.array)};
    const auto idx_val{try_eval(index_expr.index)};
    if (!target_val || !idx_val) { return stdx::none; }

    const auto idx_opt{idx_val->as_int_opt()};
    if (!idx_opt || *idx_opt < 0) {
        ctx_.diags.emplace_back("Array index must be a non-negative integer",
                                sema::error::CONSTEXPR_EVALUATION_FAILED,
                                module_.ast.location_of(index_expr.index));
        return const_value::make_poison();
    }
    const auto idx{static_cast<usize>(*idx_opt)};

    if (const auto arr{target_val->as_opt<const_array>()}) {
        if (idx >= arr->elements.size()) {
            ctx_.diags.emplace_back(
                fmt::format("Array index out of bounds: index is {}, but size is {}",
                            idx,
                            arr->elements.size()),
                sema::error::CONSTEXPR_EVALUATION_FAILED,
                module_.ast.location_of(id));
            return const_value::make_poison();
        }
        return arr->elements[idx];
    }

    if (const auto str{target_val->as_opt<std::string>()}) {
        if (idx >= str->size()) {
            ctx_.diags.emplace_back(
                fmt::format(
                    "String index out of bounds: index is {}, but size is {}", idx, str->size()),
                sema::error::CONSTEXPR_EVALUATION_FAILED,
                module_.ast.location_of(id));
            return const_value::make_poison();
        }
        return const_value{static_cast<u64>(static_cast<u8>((*str)[idx])),
                           ctx_.get_builtin_resolved_type(sema::type_kind::U8)};
    }

    return stdx::none;
}

auto const_eval::eval_initializer(ast::node_id id, const ast::initializer_expr& init)
    -> stdx::option<const_value> {
    const auto sema_type{module_.get_sema_type_opt(id)};

    if (sema_type) {
        const auto& type_data{sema_type->get_data()};
        if (type_data.is<sema::types::struct_t>()) {
            const_struct struct_val;
            for (const auto& item : init.initializers) {
                const auto& member_ident{
                    module_.ast.get_as<ast::implicit_access_expr>(item.member)};
                const auto& member_name{
                    module_.ast.get_as<ast::identifier_expr>(member_ident.member).name};
                const auto field_val{try_eval(item.value)};
                if (!field_val) { return stdx::none; }
                struct_val.fields.emplace(std::string{member_name}, *field_val);
            }
            return const_value{std::move(struct_val), sema_type};
        }
        if (type_data.is<sema::types::union_t>()) {
            if (!init.initializers.empty()) {
                const auto& item{init.initializers.front()};
                const auto& member_ident{
                    module_.ast.get_as<ast::implicit_access_expr>(item.member)};
                const auto& member_name{
                    module_.ast.get_as<ast::identifier_expr>(member_ident.member).name};
                const auto field_val{try_eval(item.value)};
                if (!field_val) { return stdx::none; }
                std::vector<const_value> payload;
                payload.emplace_back(*field_val);
                return const_value{const_union{std::string{member_name}, std::move(payload)},
                                   sema_type};
            }
        }
    }

    const_struct struct_val;
    for (const auto& item : init.initializers) {
        const auto& member_ident{module_.ast.get_as<ast::implicit_access_expr>(item.member)};
        const auto& member_name{module_.ast.get_as<ast::identifier_expr>(member_ident.member).name};
        const auto  field_val{try_eval(item.value)};
        if (!field_val) { return stdx::none; }
        struct_val.fields.emplace(std::string{member_name}, *field_val);
    }
    return const_value{std::move(struct_val), sema_type};
}

auto const_eval::eval_dot(ast::node_id, const ast::dot_expr& dot) -> stdx::option<const_value> {
    const auto& member_name{module_.ast.get_as<ast::identifier_expr>(dot.member).name};

    if (const auto obj_ident{module_.ast.get_as_opt<ast::identifier_expr>(dot.object)}) {
        if (module_.root_table_idx) {
            const auto& table{ctx_.registry.get(*module_.root_table_idx)};
            if (const auto sym{table.get_opt(obj_ident->name)}) {
                if (const auto node{sym->get_data().as_opt<sema::symbols::node_t>()}) {
                    if (const auto decl{module_.ast.get_as_opt<ast::decl_stmt>(*node)};
                        decl && decl->value) {
                        if (const auto en_expr{
                                module_.ast.get_as_opt<ast::enum_expr>(*decl->value)}) {
                            for (usize idx{0}; idx < en_expr->enumerations.size(); ++idx) {
                                const auto& e{en_expr->enumerations[idx]};
                                const auto& vname{
                                    module_.ast.get_as<ast::identifier_expr>(e.name).name};
                                if (vname == member_name) {
                                    i64 val{static_cast<i64>(idx)};
                                    if (e.value) {
                                        if (const auto ev{try_eval(*e.value)}) {
                                            val = ev->as_int_opt().value_or(val);
                                        }
                                    }
                                    return const_value{const_enum{std::string{member_name}, val},
                                                       module_.get_sema_type_opt(*decl->value)};
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    const auto target_val{try_eval(dot.object)};
    if (!target_val) { return stdx::none; }

    if (const auto st{target_val->as_opt<const_struct>()}) {
        if (const auto f{st->get_field_opt(member_name)}) { return *f; }
    }

    if (const auto un{target_val->as_opt<const_union>()}) {
        if (un->active_field == member_name && !un->payload.empty()) { return un->payload.front(); }
        ctx_.diags.emplace_back(
            fmt::format("Attempted to access inactive union field '{}' (active field is '{}')",
                        member_name,
                        un->active_field),
            sema::error::CONSTEXPR_EVALUATION_FAILED,
            module_.ast.location_of(dot.member));
        return const_value::make_poison();
    }

    return stdx::none;
}

auto const_eval::eval_implicit_access(ast::node_id id, const ast::implicit_access_expr& implicit)
    -> stdx::option<const_value> {
    const auto& member_name{module_.ast.get_as<ast::identifier_expr>(implicit.member).name};
    const auto  sema_type{module_.get_sema_type_opt(id)};

    if (sema_type) {
        if (const auto en{sema_type->get_data().as_opt<sema::types::enum_t>()}) {
            for (usize idx{0}; idx < en->ast_enumerations.size(); ++idx) {
                const auto& e{en->ast_enumerations[idx]};
                const auto& vname{module_.ast.get_as<ast::identifier_expr>(e.name).name};
                if (vname == member_name) {
                    auto val{static_cast<i64>(idx)};
                    if (e.value) {
                        if (const auto ev{try_eval(*e.value)}) {
                            val = ev->as_int_opt().value_or(val);
                        }
                    }
                    return const_value{const_enum{std::string{member_name}, val}, sema_type};
                }
            }
        }
    }

    return const_value{const_enum{std::string{member_name}, 0}, sema_type};
}

auto const_eval::eval_module_access(ast::node_id, const ast::module_access_expr& mod_access)
    -> stdx::option<const_value> {
    const auto& inner_name{module_.ast.get_as<ast::identifier_expr>(mod_access.inner).name};

    if (const auto outer_ident{module_.ast.get_as_opt<ast::identifier_expr>(mod_access.outer)}) {
        if (module_.root_table_idx) {
            const auto& table{ctx_.registry.get(*module_.root_table_idx)};
            if (const auto sym{table.get_opt(outer_ident->name)}) {
                if (const auto node{sym->get_data().as_opt<sema::symbols::node_t>()}) {
                    if (const auto decl{module_.ast.get_as_opt<ast::decl_stmt>(*node)}) {
                        if (decl->value) {
                            if (const auto en_expr{
                                    module_.ast.get_as_opt<ast::enum_expr>(*decl->value)}) {
                                for (usize idx{0}; idx < en_expr->enumerations.size(); ++idx) {
                                    const auto& e{en_expr->enumerations[idx]};
                                    const auto& vname{
                                        module_.ast.get_as<ast::identifier_expr>(e.name).name};
                                    if (vname == inner_name) {
                                        i64 val{static_cast<i64>(idx)};
                                        if (e.value) {
                                            if (const auto ev{try_eval(*e.value)}) {
                                                val = ev->as_int_opt().value_or(val);
                                            }
                                        }
                                        return const_value{const_enum{std::string{inner_name}, val},
                                                           module_.get_sema_type_opt(*decl->value)};
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return stdx::none;
}

auto const_eval::eval_match(ast::node_id id, const ast::match_expr& match)
    -> stdx::option<const_value> {
    const auto matcher_val{try_eval(match.matcher)};
    if (!matcher_val) { return stdx::none; }

    const auto eval_dispatch = [&](const ast::stmt_handle& dispatch) -> stdx::option<const_value> {
        return module_.ast[*dispatch].visit(
            [&](const auto&) -> stdx::option<const_value> { return eval_stmt(dispatch); },
            [&](const ast::expr_stmt& data) -> stdx::option<const_value> {
                return try_eval(data.expression);
            });
    };

    for (const auto& arm : match.arms) {
        if (match_pattern(arm.pattern, *matcher_val)) {
            if (arm.capture && arm.capture->template is<ast::identifier_expr>()) {
                const auto& ident{module_.ast.get_as<ast::identifier_expr>(*arm.capture)};
                if (!call_stack_.empty()) {
                    if (const auto un{matcher_val->as_opt<const_union>()}) {
                        if (!un->payload.empty()) {
                            call_stack_.back().bindings.insert_or_assign(ident.name,
                                                                         un->payload.front());
                        } else {
                            call_stack_.back().bindings.insert_or_assign(ident.name, *matcher_val);
                        }
                    } else {
                        call_stack_.back().bindings.insert_or_assign(ident.name, *matcher_val);
                    }
                }
            }
            return eval_dispatch(arm.dispatch);
        }
    }

    if (match.catch_all_idx) {
        const auto& catch_all_arm{match.arms[*match.catch_all_idx]};
        return eval_dispatch(catch_all_arm.dispatch);
    }

    ctx_.diags.emplace_back("Non-exhaustive match in compile-time constant evaluation",
                            sema::error::CONSTEXPR_EVALUATION_FAILED,
                            module_.ast.location_of(id));
    return const_value::make_poison();
}

auto const_eval::match_pattern(const ast::match_pattern_handle& pattern_h,
                               const const_value&               target) -> bool {
    const auto pattern_id{*pattern_h};

    if (pattern_h.is<ast::discarded>()) { return true; }

    if (pattern_h.is<ast::implicit_access_expr>()) {
        const auto& imp{module_.ast.get_as<ast::implicit_access_expr>(pattern_id)};
        const auto& name{module_.ast.get_as<ast::identifier_expr>(imp.member).name};
        if (const auto en{target.as_opt<const_enum>()}) { return en->name == name; }
        if (const auto un{target.as_opt<const_union>()}) { return un->active_field == name; }
        return false;
    }

    if (const auto dot{module_.ast.get_as_opt<ast::dot_expr>(pattern_id)}) {
        const auto& member_name{module_.ast.get_as<ast::identifier_expr>(dot->member).name};
        if (const auto en{target.as_opt<const_enum>()}) { return en->name == member_name; }
        if (const auto un{target.as_opt<const_union>()}) { return un->active_field == member_name; }
    }

    if (const auto range{module_.ast.get_as_opt<ast::range_expr>(pattern_id)}) {
        const auto start_val{try_eval(range->lhs)};
        const auto end_val{try_eval(range->rhs)};
        if (start_val && end_val) {
            const auto target_int{target.as_int_opt()};
            const auto start_int{start_val->as_int_opt()};
            const auto end_int{end_val->as_int_opt()};
            if (target_int && start_int && end_int) {
                return *target_int >= *start_int && *target_int < *end_int;
            }
        }
    }

    if (const auto pat_val{try_eval(pattern_id)}) { return *pat_val == target; }

    return false;
}

auto const_eval::eval_assignment(ast::node_id                id,
                                 const ast::assignment_expr& assign,
                                 syntax::token_type_t        op_type) -> stdx::option<const_value> {
    const auto ident{module_.ast.get_as_opt<ast::identifier_expr>(assign.lhs)};
    if (!ident) { return stdx::none; }

    if (op_type == syntax::token_type_t::ASSIGN) {
        const auto rhs{try_eval(assign.rhs)};
        if (!rhs) { return stdx::none; }
        if (!mutate_local_binding(ident->name, *rhs)) { return stdx::none; }
        return rhs;
    }

    if (const auto base_op{syntax::token_type::get_compound_base_op(op_type)}) {
        const auto lhs_val{try_eval(assign.lhs)};
        const auto rhs_val{try_eval(assign.rhs)};
        if (!lhs_val || !rhs_val) { return stdx::none; }

        const auto folded{fold_binary_values(*base_op, *lhs_val, *rhs_val, id)};
        if (!folded) { return stdx::none; }

        if (!mutate_local_binding(ident->name, *folded)) { return stdx::none; }
        return folded;
    }

    return stdx::none;
}

auto const_eval::fold_binary_values(syntax::token_type_t op_type,
                                    const const_value&   lhs,
                                    const const_value&   rhs,
                                    ast::node_id         id) -> stdx::option<const_value> {
    auto& bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};

    const auto on_div_zero = [&](std::string_view msg) -> const_value {
        ctx_.diags.emplace_back(std::string{msg},
                                sema::error::CONSTEXPR_EVALUATION_FAILED,
                                module_.ast.location_of(id));
        return const_value::make_poison();
    };

    if (lhs.is<f64>() || rhs.is<f64>()) {
        const auto l{lhs.is<f64>() ? lhs.as<f64>()
                                   : (lhs.is<i64>() ? static_cast<f64>(lhs.as<i64>())
                                                    : static_cast<f64>(lhs.as<u64>()))};
        const auto r{rhs.is<f64>() ? rhs.as<f64>()
                                   : (rhs.is<i64>() ? static_cast<f64>(rhs.as<i64>())
                                                    : static_cast<f64>(rhs.as<u64>()))};
        const auto res_type{lhs.get_type() ? lhs.get_type() : rhs.get_type()};
        return fold_binary_arithmetic(op_type, l, r, res_type, bool_type, on_div_zero);
    }

    const auto is_unsigned{(lhs.is<u64>() || rhs.is<u64>()) && !lhs.is<i64>()};
    if (is_unsigned) {
        const auto l{lhs.is<u64>() ? lhs.as<u64>() : static_cast<u64>(lhs.as<i64>())};
        const auto r{rhs.is<u64>() ? rhs.as<u64>() : static_cast<u64>(rhs.as<i64>())};
        const auto res_type{lhs.get_type() ? lhs.get_type() : rhs.get_type()};
        return fold_binary_arithmetic(op_type, l, r, res_type, bool_type, on_div_zero);
    }

    if ((lhs.is<i64>() || lhs.is<u64>()) && (rhs.is<i64>() || rhs.is<u64>())) {
        const auto l{lhs.is<i64>() ? lhs.as<i64>() : static_cast<i64>(lhs.as<u64>())};
        const auto r{rhs.is<i64>() ? rhs.as<i64>() : static_cast<i64>(rhs.as<u64>())};
        const auto res_type{lhs.get_type() ? lhs.get_type() : rhs.get_type()};
        return fold_binary_arithmetic(op_type, l, r, res_type, bool_type, on_div_zero);
    }

    if (lhs.is<bool>() && rhs.is<bool>()) {
        const auto l{lhs.as<bool>()};
        const auto r{rhs.as<bool>()};
        if (op_type == syntax::token_type_t::EQ) { return const_value{l == r, bool_type}; }
        if (op_type == syntax::token_type_t::NEQ) { return const_value{l != r, bool_type}; }
    }

    return stdx::none;
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

    return fold_binary_values(op_type, *lhs, *rhs, id);
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

auto const_eval::eval_ident(ast::node_id id, const ast::identifier_expr& ident)
    -> stdx::option<const_value> {
    if (auto local_val{lookup_local_binding(ident.name)}) { return local_val; }

    if (id.is_valid()) {
        if (const auto sema_type{module_.get_sema_type_opt(id)}) {
            if (sema_type->get_kind() == sema::type_kind::TYPE) {
                if (const auto meta{sema_type->get_data().as_opt<sema::types::meta_type>()}) {
                    return const_value{meta->instance};
                }
                return const_value{*sema_type};
            }
        }
    }

    if (!module_.root_table_idx) { return stdx::none; }
    const auto& table{ctx_.registry.get(*module_.root_table_idx)};
    const auto  sym_opt{table.get_opt(ident.name)};
    if (!sym_opt) {
        if (ctx_.prelude_index) {
            const auto& prelude{ctx_.registry.get(*ctx_.prelude_index)};
            if (const auto p_sym{prelude.get_opt(ident.name)}) {
                if (p_sym->get_kind() == sema::symbol_kind::TYPE) {
                    if (const auto builtin{p_sym->get_data().as_opt<sema::symbols::builtin>()}) {
                        return const_value{builtin->get_type()};
                    }
                }
            }
        }
        return stdx::none;
    }
    const auto& sym{*sym_opt};

    if (sym.get_kind() == sema::symbol_kind::TYPE) {
        if (const auto builtin_sym{sym.get_data().as_opt<sema::symbols::builtin>()}) {
            return const_value{builtin_sym->get_type()};
        }
        if (const auto node{sym.get_data().as_opt<sema::symbols::node_t>()}) {
            if (const auto using_stmt{module_.ast.get_as_opt<ast::using_stmt>(*node)}) {
                if (const auto sema_type{module_.get_sema_type_opt(using_stmt->explicit_type)}) {
                    return const_value{*sema_type};
                }
            }
            if (const auto decl{module_.ast.get_as_opt<ast::decl_stmt>(*node)}) {
                if (decl->value) {
                    const auto& node_data{module_.ast[*decl->value]};
                    if (node_data.is<ast::struct_expr>() || node_data.is<ast::enum_expr>() ||
                        node_data.is<ast::union_expr>()) {
                        if (const auto sema_type{module_.get_sema_type_opt(*decl->value)}) {
                            return const_value{*sema_type};
                        }
                    }
                    return try_eval(*decl->value);
                }
            }
        }
    }

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
    const auto fn_token{call.function->get_token_type()};
    if (syntax::get_builtin_opt(fn_token)) { return eval_builtin(call, fn_token); }

    if (const auto ident{module_.ast.get_as_opt<ast::identifier_expr>(call.function)}) {
        if (!module_.root_table_idx) { return stdx::none; }
        const auto& table{ctx_.registry.get(*module_.root_table_idx)};

        const auto sym{table.get_opt(ident->name)};
        if (!sym) { return stdx::none; }
        const auto node{sym->get_data().as_opt<sema::symbols::node_t>()};
        if (!node) { return stdx::none; }
        const auto decl{module_.ast.get_as_opt<ast::decl_stmt>(*node)};
        if (!decl) { return stdx::none; }

        if ((decl->has_modifier(ast::decl_modifiers::CONSTEXPR) ||
             decl->has_modifier(ast::decl_modifiers::CONSTANT)) &&
            decl->value) {
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
    case syntax::token_type_t::BUILTIN_THIS: {
        if (const auto target_type{module_.get_sema_type_opt(call.function)}) {
            return const_value{target_type};
        }
        return stdx::none;
    }
    case syntax::token_type_t::BUILTIN_SIZE_OF: {
        VERIFY(!call.arguments.empty(), "@sizeOf argument count must be at least 1");
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
        VERIFY(!call.arguments.empty(), "@alignOf argument count must be at least 1");
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
        VERIFY(!call.arguments.empty(), "@typeOf argument count must be at least 1");
        const auto& arg{call.arguments.front()};
        if (const auto expr_h{arg.as_opt<ast::expr_handle>()}) {
            const auto target_type{module_.get_sema_type_opt(*expr_h)};
            if (target_type) { return const_value{target_type}; }
        }
        return stdx::none;
    }
    case syntax::token_type_t::BUILTIN_ABS: {
        VERIFY(!call.arguments.empty(), "@abs argument count must be at least 1");
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
        VERIFY(!call.arguments.empty(), "@sqrt argument count must be at least 1");
        const auto expr_h{call.arguments.front().as_opt<ast::expr_handle>()};
        if (!expr_h) { return stdx::none; }
        const auto arg{try_eval(*expr_h)};
        if (!arg || !arg->is<f64>()) { return stdx::none; }
        return const_value{std::sqrt(arg->as<f64>()), arg->get_type()};
    }
    case syntax::token_type_t::BUILTIN_FLOOR: {
        VERIFY(!call.arguments.empty(), "@floor argument count must be at least 1");
        const auto expr_h{call.arguments.front().as_opt<ast::expr_handle>()};
        if (!expr_h) { return stdx::none; }
        const auto arg{try_eval(*expr_h)};
        if (!arg || !arg->is<f64>()) { return stdx::none; }
        return const_value{std::floor(arg->as<f64>()), arg->get_type()};
    }
    case syntax::token_type_t::BUILTIN_CEIL: {
        VERIFY(!call.arguments.empty(), "@ceil argument count must be at least 1");
        const auto expr_h{call.arguments.front().as_opt<ast::expr_handle>()};
        if (!expr_h) { return stdx::none; }
        const auto arg{try_eval(*expr_h)};
        if (!arg || !arg->is<f64>()) { return stdx::none; }
        return const_value{std::ceil(arg->as<f64>()), arg->get_type()};
    }
    case syntax::token_type_t::BUILTIN_CLZ: {
        VERIFY(!call.arguments.empty(), "@clz argument count must be at least 1");
        const auto expr_h{call.arguments.front().as_opt<ast::expr_handle>()};
        if (!expr_h) { return stdx::none; }
        const auto arg{try_eval(*expr_h)};
        if (!arg) { return stdx::none; }
        if (!arg->is<u64>() && !arg->is<i64>()) { return stdx::none; }
        const auto v{arg->is<u64>() ? arg->as<u64>() : static_cast<u64>(arg->as<i64>())};
        return const_value{static_cast<u64>(std::countl_zero(v)), usize_type};
    }
    case syntax::token_type_t::BUILTIN_CTZ: {
        VERIFY(!call.arguments.empty(), "@ctz argument count must be at least 1");
        const auto expr_h{call.arguments.front().as_opt<ast::expr_handle>()};
        if (!expr_h) { return stdx::none; }
        const auto arg{try_eval(*expr_h)};
        if (!arg) { return stdx::none; }
        if (!arg->is<u64>() && !arg->is<i64>()) { return stdx::none; }
        const auto v{arg->is<u64>() ? arg->as<u64>() : static_cast<u64>(arg->as<i64>())};
        return const_value{static_cast<u64>(std::countr_zero(v)), usize_type};
    }
    case syntax::token_type_t::BUILTIN_POP_COUNT: {
        VERIFY(!call.arguments.empty(), "@popCount argument count must be at least 1");
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
    VERIFY(fn_expr.parameters.size() == args.size(),
           "Constexpr function args count must match parameters count");

    if (recursion_depth_ >= max_recursion_depth_) {
        const auto loc{call_id.is_valid() ? module_.ast.location_of(call_id)
                                          : module_.ast.location_of(fn_expr.body)};
        ctx_.diags.emplace_back("Constexpr function recursion limit exceeded",
                                sema::error::CONSTEXPR_RECURSION_LIMIT_EXCEEDED,
                                loc);
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
            const auto expr_id{data.expression};
            if (module_.ast[expr_id].template is<ast::if_expr>() ||
                module_.ast[expr_id].template is<ast::while_loop_expr>() ||
                module_.ast[expr_id].template is<ast::do_while_loop_expr>() ||
                module_.ast[expr_id].template is<ast::for_loop_expr>() ||
                module_.ast[expr_id].template is<ast::match_expr>()) {
                return try_eval(expr_id);
            }
            static_cast<void>(try_eval(expr_id));
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
            call_stack_.back().bindings.insert_or_assign(ident.name, *val);
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

auto const_eval::eval_while(ast::node_id, const ast::while_loop_expr& loop)
    -> stdx::option<const_value> {
    while (true) {
        const auto cond{try_eval(loop.condition)};
        if (!cond || !cond->is<bool>()) { return stdx::none; }
        if (!cond->as<bool>()) { break; }

        if (const auto body_res{eval_stmt(loop.block)}) { return body_res; }

        if (loop.continuation) { static_cast<void>(try_eval(*loop.continuation)); }
    }
    return stdx::none;
}

auto const_eval::eval_do_while(ast::node_id, const ast::do_while_loop_expr& loop)
    -> stdx::option<const_value> {
    while (true) {
        if (const auto body_res{eval_stmt(loop.block)}) { return body_res; }

        const auto cond{try_eval(loop.condition)};
        if (!cond || !cond->is<bool>()) { return stdx::none; }
        if (!cond->as<bool>()) { break; }
    }
    return stdx::none;
}

auto const_eval::eval_for(ast::node_id, const ast::for_loop_expr& loop)
    -> stdx::option<const_value> {
    VERIFY(loop.iterables.size() == loop.captures.size(),
           "For loop iterables and captures must match in count");
    if (loop.iterables.empty()) { return stdx::none; }

    std::vector<std::vector<const_value>> iterable_sequences;
    iterable_sequences.reserve(loop.iterables.size());

    for (const auto& iterable_handle : loop.iterables) {
        std::vector<const_value> sequence;
        const auto               iterable_id{*iterable_handle};

        if (const auto range{module_.ast.get_as_opt<ast::range_expr>(iterable_id)}) {
            const auto start_val{try_eval(range->lhs)};
            const auto end_val{try_eval(range->rhs)};
            if (!start_val || !end_val) { return stdx::none; }

            const auto start_opt{start_val->as_int_opt()};
            const auto end_opt{end_val->as_int_opt()};
            if (!start_opt || !end_opt) { return stdx::none; }

            const auto start{*start_opt};
            const auto end{*end_opt};
            const auto target_type{start_val->get_type()};

            for (auto i{start}; i < end; ++i) {
                if (start_val->is<u64>()) {
                    sequence.emplace_back(static_cast<u64>(i), target_type);
                } else {
                    sequence.emplace_back(static_cast<i64>(i), target_type);
                }
            }
        } else if (const auto array{module_.ast.get_as_opt<ast::array_expr>(iterable_id)}) {
            for (const auto& item_h : array->items) {
                const auto item_val{try_eval(item_h)};
                if (!item_val) { return stdx::none; }
                sequence.emplace_back(*item_val);
            }
        } else {
            const auto val{try_eval(iterable_id)};
            if (val) {
                if (const auto arr{val->as_opt<const_array>()}) {
                    sequence = arr->elements;
                } else {
                    return stdx::none;
                }
            } else {
                return stdx::none;
            }
        }

        iterable_sequences.emplace_back(std::move(sequence));
    }

    usize iter_count{iterable_sequences.front().size()};
    for (const auto& seq : iterable_sequences) { iter_count = std::min(iter_count, seq.size()); }

    for (usize step{0}; step < iter_count; ++step) {
        for (usize idx{0}; idx < loop.captures.size(); ++idx) {
            const auto& capture{loop.captures[idx]};
            if (capture.payload.template is<ast::identifier_expr>()) {
                const auto& ident{module_.ast.get_as<ast::identifier_expr>(capture.payload)};
                if (!call_stack_.empty()) {
                    call_stack_.back().bindings.insert_or_assign(ident.name,
                                                                 iterable_sequences[idx][step]);
                }
            }
        }

        if (const auto body_res{eval_stmt(loop.block)}) { return body_res; }
    }

    if (loop.non_break) { return eval_stmt(*loop.non_break); }

    return stdx::none;
}

} // namespace ghoti::gir
