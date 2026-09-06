#include "compiler/gir/const_eval.hh"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <llvm/TargetParser/Triple.h>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/string.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/type.hh"
#include "compiler/codegen/target.hh"
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
#include "support/int128.hh"

namespace ghoti::gir {

namespace {

// Compile-time integer results evaluate at 128-bit width but store in the narrowest
// arm that holds them, so the common (<=64-bit) case is unchanged for every consumer.
template <typename T>
[[nodiscard]] auto make_scalar_const(T v, stdx::option<sema::type&> t) -> const_value {
    if constexpr (std::is_floating_point_v<T>) {
        return const_value{v, t};
    } else if constexpr (Signed<T>) {
        const auto w{static_cast<i128>(v)};
        if (w >= static_cast<i128>(std::numeric_limits<i64>::min()) &&
            w <= static_cast<i128>(std::numeric_limits<u64>::max())) {
            return const_value{static_cast<i64>(w), t};
        }
        return const_value{w, t};
    } else {
        const auto w{static_cast<u128>(v)};
        if (w <= static_cast<u128>(std::numeric_limits<u64>::max())) {
            return const_value{static_cast<u64>(w), t};
        }
        return const_value{w, t};
    }
}

template <typename T>
[[nodiscard]] auto fold_binary_arithmetic(syntax::token_type_t      op_type,
                                          T                         l,
                                          T                         r,
                                          stdx::option<sema::type&> res_type,
                                          sema::type&               bool_type,
                                          auto&& on_div_zero) -> stdx::option<const_value> {
    switch (op_type) {
    case syntax::token_type_t::PLUS:  return make_scalar_const(l + r, res_type);
    case syntax::token_type_t::MINUS: return make_scalar_const(l - r, res_type);
    case syntax::token_type_t::STAR:  return make_scalar_const(l * r, res_type);
    case syntax::token_type_t::SLASH:
        if (r == 0) { return on_div_zero("Division by zero in compile-time constant expression"); }
        return make_scalar_const(l / r, res_type);
    case syntax::token_type_t::PERCENT:
        if constexpr (Integral<T>) {
            if (r == 0) {
                return on_div_zero("Modulo by zero in compile-time constant expression");
            }
            return make_scalar_const(l % r, res_type);
        } else {
            return stdx::none;
        }
    case syntax::token_type_t::BW_AND:
        if constexpr (Integral<T>) { return make_scalar_const(l & r, res_type); }
        return stdx::none;
    case syntax::token_type_t::BW_OR:
        if constexpr (Integral<T>) { return make_scalar_const(l | r, res_type); }
        return stdx::none;
    case syntax::token_type_t::CARET:
        if constexpr (Integral<T>) { return make_scalar_const(l ^ r, res_type); }
        return stdx::none;
    case syntax::token_type_t::SHL:
        if constexpr (Integral<T>) { return make_scalar_const(l << r, res_type); }
        return stdx::none;
    case syntax::token_type_t::SHR:
        if constexpr (Integral<T>) { return make_scalar_const(l >> r, res_type); }
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

// The plain base op a wrapping token folds through
[[nodiscard]] constexpr auto wrapping_base_op(syntax::token_type_t tok) noexcept
    -> stdx::option<syntax::token_type_t> {
    switch (tok) {
    case syntax::token_type_t::PLUS_PERCENT:  return syntax::token_type_t::PLUS;
    case syntax::token_type_t::MINUS_PERCENT: return syntax::token_type_t::MINUS;
    case syntax::token_type_t::STAR_PERCENT:  return syntax::token_type_t::STAR;
    case syntax::token_type_t::SHL_PERCENT:   return syntax::token_type_t::SHL;
    default:                                  return stdx::none;
    }
}

// Truncates `folded`'s integer value to `bits` (two's-complement), rebuilding it at `res_type`
[[nodiscard]] auto wrap_to_width(const const_value&        folded,
                                 u16                       bits,
                                 bool                      is_signed,
                                 stdx::option<sema::type&> res_type) -> const_value {
    // A no-op past 128 bits: the fold already happened in the 128-bit comptime domain
    if (bits == 0 || bits >= 128) { return folded; }
    const u128 mask{(u128{1} << bits) - 1};
    // Two's-complement bit pattern of the source, valid for negative operands too
    const auto raw{static_cast<u128>(folded.as_int_opt().value_or(0))};
    if (!is_signed) { return make_scalar_const(raw & mask, res_type); }
    const auto masked{raw & mask};
    const u128 sign_bit{u128{1} << (bits - 1)};
    if (masked & sign_bit) {
        return make_scalar_const(static_cast<i128>(masked) - static_cast<i128>(mask) - i128{1},
                                 res_type);
    }
    return make_scalar_const(static_cast<i128>(masked), res_type);
}

// `{bit width, is signed}` for a concrete integer target (`iN`/`uN`/`isize`/`usize`),
// or none for any other kind. `isize`/`usize` resolve against the target pointer width.
[[nodiscard]] auto integer_target_width(const sema::type& t, u32 ptr_bits)
    -> stdx::option<std::pair<u16, bool>> {
    switch (t.get_kind()) {
    case sema::type_kind::INT:   return std::pair{sema::int_width(t), sema::is_signed_integer(t)};
    case sema::type_kind::ISIZE: return std::pair{static_cast<u16>(ptr_bits), true};
    case sema::type_kind::USIZE: return std::pair{static_cast<u16>(ptr_bits), false};
    default:                     return stdx::none;
    }
}

} // namespace

auto const_eval::try_eval(ast::node_id id) -> stdx::option<const_value> {
    PROFILE_FUNCTION();
    if (const auto sema_ty{module_->get_sema_type_opt(id)}) {
        if (sema_ty->is_volatile()) { return stdx::none; }
    }
    const auto key{id.get_index()};
    const bool is_outermost{call_stack_.empty()};
    if (is_outermost) {
        if (auto cached{memo_cache_.find(key)}; cached != memo_cache_.end()) {
            return cached->second;
        }
        // A top-level const initializer has no call frame; push a synthetic one to bind into.
        call_stack_.emplace_back();
    }

    auto res{eval_node(id)};
    if (res && is_outermost) { memo_cache_.emplace(key, *res); }
    if (is_outermost) { call_stack_.pop_back(); }
    return res;
}

auto const_eval::eval(ast::node_id id) -> const_value {
    PROFILE_FUNCTION();
    auto res{try_eval(id)};
    if (!res || res->is_poison()) {
        ctx_.diags.emplace_back("Expression cannot be evaluated as a compile-time constant",
                                sema::error::CONSTEXPR_EVALUATION_FAILED,
                                module_->ast.location_of(id));
        return const_value::make_poison();
    }
    return *res;
}

auto const_eval::eval_type_dim(ast::node_id id) -> stdx::option<usize> {
    PROFILE_FUNCTION();
    const auto val{eval(id)};
    if (val.is_poison()) { return stdx::none; }

    if (const auto dim{val.as_opt<u64>()}) { return static_cast<usize>(*dim); }
    if (const auto dim{val.as_opt<i64>()}) {
        if (*dim < 0) {
            ctx_.diags.emplace_back("Array dimension cannot be negative",
                                    sema::error::CONSTEXPR_EVALUATION_FAILED,
                                    module_->ast.location_of(id));
            return stdx::none;
        }
        return static_cast<usize>(*dim);
    }

    ctx_.diags.emplace_back("Array dimension must evaluate to an integer constant",
                            sema::error::CONSTEXPR_EVALUATION_FAILED,
                            module_->ast.location_of(id));
    return stdx::none;
}

auto const_eval::resolve_all_deferred_calls() -> void {
    PROFILE_FUNCTION();
    for (auto& type_opt : module_->sema_side_tables.explicit_types.values) {
        if (!type_opt) { continue; }
        if (const auto def{type_opt->get_data().as_opt<sema::types::deferred_call>()}) {
            type_opt.emplace(resolve_deferred_call(def->call));
        }
    }

    for (auto& type_opt : module_->sema_side_tables.node_types.values) {
        if (!type_opt) { continue; }
        if (const auto def{type_opt->get_data().as_opt<sema::types::deferred_call>()}) {
            type_opt.emplace(resolve_deferred_call(def->call));
        }
    }
}

auto const_eval::resolve_all_deferred_arrays() -> void {
    PROFILE_FUNCTION();
    resolve_all_deferred_calls();

    for (auto& type_opt : module_->sema_side_tables.explicit_types.values) {
        if (!type_opt) { continue; }
        if (const auto def{type_opt->get_data().as_opt<sema::types::deferred_array>()}) {
            if (auto concrete{resolve_deferred_array(def->array, def->underlying)}) {
                type_opt.emplace(*concrete);
            }
        }
        force_deferred_function_params(*type_opt);
        force_deferred_aggregate_fields(*type_opt);
        force_deferred_indirection_underlying(*type_opt);
    }

    for (auto& type_opt : module_->sema_side_tables.node_types.values) {
        if (!type_opt) { continue; }
        if (const auto def{type_opt->get_data().as_opt<sema::types::deferred_array>()}) {
            if (auto concrete{resolve_deferred_array(def->array, def->underlying)}) {
                type_opt.emplace(*concrete);
            }
        }
        force_deferred_function_params(*type_opt);
        force_deferred_aggregate_fields(*type_opt);
        force_deferred_indirection_underlying(*type_opt);
    }
}

auto const_eval::resolve_all_deferred_types() -> void { resolve_all_deferred_arrays(); }

namespace {

[[nodiscard]] auto int_abi_bytes(u16 bits) -> usize {
    if (bits <= 8) { return 1; }
    if (bits <= 16) { return 2; }
    if (bits <= 32) { return 4; }
    if (bits <= 64) { return 8; }
    usize bytes{1};
    while (bytes * 8 < bits) { bytes *= 2; }
    return bytes;
}

[[nodiscard]] auto capture_field_align(const sema::types::closure_capture& capture, usize ptr_size)
    -> usize {
    return const_eval::type_align_of(*capture.storage_type, ptr_size);
}

[[nodiscard]] auto capture_field_size(const sema::types::closure_capture& capture, usize ptr_size)
    -> usize {
    return const_eval::type_size_of(*capture.storage_type, ptr_size);
}

} // namespace

auto const_eval::type_align_of(const sema::type& type, usize ptr_size) -> usize {
    PROFILE_FUNCTION();
    switch (type.get_kind()) {
    case sema::type_kind::INT:             return int_abi_bytes(sema::int_width(type));
    case sema::type_kind::BOOL:            return 1;
    case sema::type_kind::F16:             return 2;
    case sema::type_kind::F32:             return 4;
    case sema::type_kind::CONSTEXPR_INT:   return 4; // materializes as i32
    case sema::type_kind::F64:
    case sema::type_kind::CONSTEXPR_FLOAT: return 8; // materializes as f64
    case sema::type_kind::F80:
    case sema::type_kind::F128:            return 16;
    case sema::type_kind::ISIZE:
    case sema::type_kind::USIZE:
    case sema::type_kind::POINTER:
    case sema::type_kind::REFERENCE:
    case sema::type_kind::FUNCTION:
    case sema::type_kind::SLICE:           return ptr_size;
    case sema::type_kind::ARRAY:
        if (const auto arr{type.get_data().as_opt<sema::types::array>()}) {
            return type_align_of(arr->underlying, ptr_size);
        }
        if (const auto def{type.get_data().as_opt<sema::types::deferred_array>()}) {
            return type_align_of(def->underlying, ptr_size);
        }
        UNREACHABLE("type_kind::ARRAY associated with improper type");
    case sema::type_kind::STRUCT:
        if (const auto st{type.get_data().as_opt<sema::types::struct_t>()}) {
            if (st->is_bit_packed()) {
                if (const auto bits{
                        sema::packed_backing_bits(*st, static_cast<u32>(ptr_size) * 8)}) {
                    return int_abi_bytes(static_cast<u16>(*bits));
                }
            }
            return std::ranges::fold_left(st->fields | std::views::filter([](const auto* f) {
                                              return f != nullptr;
                                          }) | std::views::transform([ptr_size](const auto* f) {
                                              return type_align_of(*f, ptr_size);
                                          }),
                                          1UZ,
                                          [](usize a, usize b) { return std::max(a, b); });
        }
        UNREACHABLE("type_kind::STRUCT associated with improper type");
    case sema::type_kind::UNION:
        if (const auto un{type.get_data().as_opt<sema::types::union_t>()}) {
            if (un->is_bit_packed()) {
                if (const auto bits{
                        sema::packed_union_backing_bits(*un, static_cast<u32>(ptr_size) * 8)}) {
                    return int_abi_bytes(static_cast<u16>(*bits));
                }
            }
            return std::ranges::fold_left(un->fields | std::views::filter([](const auto* f) {
                                              return f != nullptr;
                                          }) | std::views::transform([ptr_size](const auto* f) {
                                              return type_align_of(*f, ptr_size);
                                          }),
                                          1UZ,
                                          [](usize a, usize b) { return std::max(a, b); });
        }
        UNREACHABLE("type_kind::UNION associated with improper type");
    case sema::type_kind::ENUM:
        if (const auto en{type.get_data().as_opt<sema::types::enum_t>()}) {
            return type_align_of(en->underlying, ptr_size);
        }
        UNREACHABLE("type_kind::ENUM associated with improper type");
    case sema::type_kind::CLOSURE:
        if (const auto cl{type.get_data().as_opt<sema::types::closure_t>()}) {
            return std::ranges::fold_left(cl->captures |
                                              std::views::transform([ptr_size](const auto& c) {
                                                  return capture_field_align(c, ptr_size);
                                              }),
                                          1UZ,
                                          [](usize a, usize b) { return std::max(a, b); });
        }
        UNREACHABLE("type_kind::CLOSURE associated with improper type");
    default: return 1;
    }
}

auto const_eval::type_size_of(const sema::type& type, usize ptr_size) -> usize {
    PROFILE_FUNCTION();
    switch (type.get_kind()) {
    case sema::type_kind::VOID_:           return 0;
    case sema::type_kind::INT:             return int_abi_bytes(sema::int_width(type));
    case sema::type_kind::BOOL:            return 1;
    case sema::type_kind::F16:             return 2;
    case sema::type_kind::F32:             return 4;
    case sema::type_kind::CONSTEXPR_INT:   return 4; // materializes as i32
    case sema::type_kind::F64:
    case sema::type_kind::CONSTEXPR_FLOAT: return 8; // materializes as f64
    case sema::type_kind::F80:
    case sema::type_kind::F128:            return 16;
    case sema::type_kind::ISIZE:
    case sema::type_kind::USIZE:
    case sema::type_kind::POINTER:
    case sema::type_kind::REFERENCE:
    case sema::type_kind::FUNCTION:        return ptr_size;
    case sema::type_kind::SLICE:           return 2 * ptr_size;
    case sema::type_kind::ARRAY:
        if (const auto arr{type.get_data().as_opt<sema::types::array>()}) {
            const auto elem_size{type_size_of(arr->underlying, ptr_size)};
            const auto elem_align{type_align_of(arr->underlying, ptr_size)};
            const auto elem_stride{elem_align > 0
                                       ? (elem_size + elem_align - 1) / elem_align * elem_align
                                       : elem_size};
            return arr->len * (elem_stride == 0 ? elem_size : elem_stride);
        }
        UNREACHABLE("type_kind::ARRAY associated with improper type");
    case sema::type_kind::STRUCT:
        if (const auto st{type.get_data().as_opt<sema::types::struct_t>()}) {
            if (st->is_bit_packed()) {
                if (const auto bits{
                        sema::packed_backing_bits(*st, static_cast<u32>(ptr_size) * 8)}) {
                    return int_abi_bytes(static_cast<u16>(*bits));
                }
            }
            usize current_offset{0};
            usize max_align{1};
            for (const auto* field : st->fields) {
                if (!field) { continue; }
                const auto f_align{type_align_of(*field, ptr_size)};
                max_align = std::max(max_align, f_align);
                if (f_align > 0) {
                    current_offset = (current_offset + f_align - 1) / f_align * f_align;
                }
                current_offset += type_size_of(*field, ptr_size);
            }

            return max_align > 0 ? (current_offset + max_align - 1) / max_align * max_align
                                 : current_offset;
        }
        UNREACHABLE("type_kind::STRUCT associated with improper type");
    case sema::type_kind::UNION:
        if (const auto un{type.get_data().as_opt<sema::types::union_t>()}) {
            if (un->is_bit_packed()) {
                if (const auto bits{
                        sema::packed_union_backing_bits(*un, static_cast<u32>(ptr_size) * 8)}) {
                    return int_abi_bytes(static_cast<u16>(*bits));
                }
            }
            usize max_size{0};
            usize max_align{1};
            for (const auto* field : un->fields) {
                if (!field) { continue; }
                max_align = std::max(max_align, type_align_of(*field, ptr_size));
                max_size  = std::max(max_size, type_size_of(*field, ptr_size));
            }
            return max_align > 0 ? (max_size + max_align - 1) / max_align * max_align : max_size;
        }
        UNREACHABLE("type_kind::UNION associated with improper type");
    case sema::type_kind::ENUM:
        if (const auto en{type.get_data().as_opt<sema::types::enum_t>()}) {
            return type_size_of(en->underlying, ptr_size);
        }
        UNREACHABLE("type_kind::ENUM associated with improper type");
    case sema::type_kind::CLOSURE:
        if (const auto cl{type.get_data().as_opt<sema::types::closure_t>()}) {
            usize current_offset{0};
            usize max_align{1};
            for (const auto& capture : cl->captures) {
                const auto c_align{capture_field_align(capture, ptr_size)};
                max_align = std::max(max_align, c_align);
                if (c_align > 0) {
                    current_offset = (current_offset + c_align - 1) / c_align * c_align;
                }
                current_offset += capture_field_size(capture, ptr_size);
            }
            return max_align > 0 ? (current_offset + max_align - 1) / max_align * max_align
                                 : current_offset;
        }
        UNREACHABLE("type_kind::CLOSURE associated with improper type");
    default: return 0;
    }
}

auto const_eval::force_deferred_array(sema::type& maybe_deferred) -> sema::type& {
    PROFILE_FUNCTION();
    const auto deferred{maybe_deferred.get_data().as_opt<sema::types::deferred_array>()};
    if (!deferred) { return maybe_deferred; }
    if (auto concrete{resolve_deferred_array(deferred->array, deferred->underlying)}) {
        return *concrete;
    }
    return maybe_deferred;
}

auto const_eval::force_deferred_array_elements(gsl::span<sema::type*> elements) -> void {
    PROFILE_FUNCTION();
    for (auto& element : elements) { element = &force_deferred_array(*element); }
}

auto const_eval::force_deferred_aggregate_fields(sema::type& maybe_aggregate) -> void {
    PROFILE_FUNCTION();
    // A type re-exported from another module can surface in this module's
    // side tables, but its field AST nodes index into the declaring module's pools
    if (const auto st{maybe_aggregate.get_data().as_opt<sema::types::struct_t>()}) {
        if (&st->enclosing != module_) { return; }
        force_deferred_array_elements(st->fields);
        return;
    }
    if (const auto ut{maybe_aggregate.get_data().as_opt<sema::types::union_t>()}) {
        if (&ut->enclosing != module_) { return; }
        force_deferred_array_elements(ut->fields);
    }
}

auto const_eval::force_deferred_indirection_underlying(sema::type& maybe_indirection) -> void {
    PROFILE_FUNCTION();
    if (const auto ref_data{maybe_indirection.get_data().as_opt<sema::types::reference>()}) {
        auto& underlying{const_cast<sema::type&>(ref_data->underlying)};
        maybe_indirection.resolve<sema::types::reference>(force_deferred_array(underlying));
        return;
    }
    if (const auto ptr_data{maybe_indirection.get_data().as_opt<sema::types::pointer>()}) {
        auto& underlying{const_cast<sema::type&>(ptr_data->underlying)};
        maybe_indirection.resolve<sema::types::pointer>(force_deferred_array(underlying));
    }
}

auto const_eval::force_deferred_function_params(sema::type& maybe_fn) -> void {
    PROFILE_FUNCTION();
    const auto fn_data{maybe_fn.get_data().as_opt<sema::types::function>()};
    if (!fn_data) { return; }

    // Copy the fields out before `resolve` re-emplaces `data_` and invalidates `fn_data`.
    const auto params{fn_data->params};
    const auto has_self{fn_data->has_self};
    const auto is_variadic{fn_data->is_variadic};
    force_deferred_array_elements(params);
    auto& return_type{force_deferred_array(fn_data->return_type)};
    maybe_fn.resolve<sema::types::function>(params, return_type, has_self, is_variadic);
}

auto const_eval::resolve_deferred_array(const ast::explicit_array_type& array,
                                        sema::type& item_type) -> stdx::option<sema::type&> {
    PROFILE_FUNCTION();
    ASSERT(array.dimension, "Deferred array type must have a dimension");
    // `array` can belong to a different module's AST when a type is re-exported across modules;
    // its dimension node only indexes that module's tables. Leave the type deferred here rather
    // than indexing ours out of bounds - the declaring module's own pass resolves it.
    if (array.dimension->get_index() >= module_->sema_side_tables.node_types.values.size()) {
        return stdx::none;
    }
    const auto cv{try_eval(*array.dimension)};
    if (!cv || cv->is_poison()) { return stdx::none; }
    const auto len{cv->as_uint_opt().value_or(0)};
    const auto mutability{array.mut_elements ? sema::types::mut::MUTABLE
                                             : sema::types::mut::CONSTANT};
    return ctx_.get_array(mutability, array.null_terminated, static_cast<usize>(len), item_type);
}

auto const_eval::try_resolve_deferred_call(const ast::call_expr& call)
    -> stdx::option<sema::type&> {
    PROFILE_FUNCTION();
    const auto val{eval_call(ast::node_id::make_invalid(), call)};
    if (!val) { return stdx::none; }
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
    return stdx::none;
}

auto const_eval::resolve_deferred_call(const ast::call_expr& call) -> sema::type& {
    PROFILE_FUNCTION();
    if (const auto resolved{try_resolve_deferred_call(call)}) { return *resolved; }
    ctx_.diags.emplace_back("Failed to evaluate compile-time type constructor function",
                            sema::error::CONSTEXPR_EVALUATION_FAILED,
                            module_->ast.location_of(call.function));
    return ctx_.get_poison();
}

auto const_eval::force_deferred_call(sema::type& maybe_deferred) -> sema::type& {
    PROFILE_FUNCTION();
    const auto deferred{maybe_deferred.get_data().as_opt<sema::types::deferred_call>()};
    if (!deferred) { return maybe_deferred; }
    const auto resolved{try_resolve_deferred_call(deferred->call)};
    if (!resolved) { return maybe_deferred; }
    // Only an aggregate result needs to be pinned early
    switch (resolved->get_kind()) {
    case sema::type_kind::STRUCT:
    case sema::type_kind::UNION:
    case sema::type_kind::ENUM:   return *resolved;
    default:                      return maybe_deferred;
    }
}

auto const_eval::lookup_local_binding(std::string_view name) const noexcept
    -> stdx::option<const_value> {
    for (auto& frame : call_stack_ | std::views::reverse) {
        if (auto it{frame.bindings.find(name)}; it != frame.bindings.end()) { return it->second; }
    }
    return stdx::none;
}

auto const_eval::set_local_binding(std::string_view name, const_value val) -> bool {
    PROFILE_FUNCTION();
    for (auto& frame : call_stack_ | std::views::reverse) {
        if (auto it{frame.bindings.find(name)}; it != frame.bindings.end()) {
            it->second = std::move(val);
            return true;
        }
    }
    return false;
}

auto const_eval::eval_node(ast::node_id id) -> stdx::option<const_value> {
    PROFILE_FUNCTION();
    if (!id.is_valid()) { return stdx::none; }

    return module_->ast[id].visit(
        [](const auto&) -> stdx::option<const_value> { return stdx::none; },
        [&](const ast::int_literal_expr& data) -> stdx::option<const_value> {
            const auto sema_type{module_->get_sema_type_opt(id)};
            auto&      t{sema_type ? *sema_type : ctx_.get_int(32, true)};
            // An unsuffixed integer literal used in a float context folds to a float value.
            if (sema::is_float(t.get_kind()) || t.get_kind() == sema::type_kind::CONSTEXPR_FLOAT) {
                if (t.get_kind() == sema::type_kind::F80 || t.get_kind() == sema::type_kind::F128) {
                    return stdx::none;
                }
                return make_scalar_const(static_cast<f64>(static_cast<i128>(data.value)), t);
            }
            if (sema::is_unsigned_integer(t)) {
                return make_scalar_const(static_cast<u128>(data.value), t);
            }
            return make_scalar_const(static_cast<i128>(data.value), t);
        },
        [&](const ast::float_literal_expr& data) -> stdx::option<const_value> {
            // `f80`/`f128` cannot be represented exactly at compile time; refuse to fold
            if (data.width == 80 || data.width == 128) { return stdx::none; }
            const auto sema_type{module_->get_sema_type_opt(id)};
            if (sema_type && (sema_type->get_kind() == sema::type_kind::F80 ||
                              sema_type->get_kind() == sema::type_kind::F128)) {
                return stdx::none;
            }
            return const_value{data.value,
                               sema_type ? *sema_type
                                         : ctx_.get_builtin_resolved_type(sema::type_kind::F64)};
        },
        [&](ast::bool_expr) {
            return const_value{id.get_token_type() == syntax::token_type_t::BOOLEAN_TRUE,
                               ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
        },
        [&](const ast::string_expr& data) {
            return const_value{std::string{data.value}, module_->get_sema_type_opt(id)};
        },
        [&](ast::void_expr) {
            return const_value{void_val{}, ctx_.get_builtin_resolved_type(sema::type_kind::VOID_)};
        },
        [&](ast::undefined_expr) {
            return const_value{undefined_val{},
                               ctx_.get_builtin_resolved_type(sema::type_kind::UNDEFINED)};
        },
        [&](ast::nullptr_expr) {
            return const_value{nullptr_val{},
                               ctx_.get_builtin_resolved_type(sema::type_kind::NULLPTR)};
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
        [&](const ast::unwrap_expr& data) { return eval_unwrap(id, data); },
        [&](const ast::assignment_expr& data) {
            return eval_assignment(id, data, id.get_token_type());
        },
        [&](const ast::binary_expr& data) { return eval_binary(id, data); },
        [&](const ast::unary_expr& data) { return eval_unary(id, data); },
        [&](const ast::identifier_expr& data) { return eval_ident(id, data); },
        [&](const ast::call_expr& data) { return eval_call(id, data); },
        [&](const ast::if_expr& data) { return eval_if(id, data); },
        [&](const ast::cfg_value_expr&) -> stdx::option<const_value> {
            // The cfg pass already settled this; read its verdict.
            const auto it{module_->cfg_value_results.find(id.get_index())};
            if (it == module_->cfg_value_results.end()) { return stdx::none; }
            if (it->second.is_predicate) {
                return const_value{it->second.boolean,
                                   ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
            }
            return it->second.chosen.is_valid() ? try_eval(it->second.chosen) : stdx::none;
        },
        [&](const ast::while_loop_expr& data) { return eval_while(id, data); },
        [&](const ast::do_while_loop_expr& data) { return eval_do_while(id, data); },
        [&](const ast::for_loop_expr& data) { return eval_for(id, data); },
        [&](const ast::struct_expr&) { return const_value{module_->get_sema_type_opt(id)}; },
        [&](const ast::enum_expr&) { return const_value{module_->get_sema_type_opt(id)}; },
        [&](const ast::union_expr&) { return const_value{module_->get_sema_type_opt(id)}; },
        [&](const ast::dereference_expr& data) -> stdx::option<const_value> {
            if (const auto ref{module_->ast.get_as_opt<ast::reference_expr>(data.rhs)}) {
                return try_eval(ref->rhs);
            }
            if (const auto addr{module_->ast.get_as_opt<ast::address_of_expr>(data.rhs)}) {
                return try_eval(addr->rhs);
            }
            return try_eval(data.rhs);
        },
        // A reference / pointer value is an address, not a compile-time constant
        [&](const ast::reference_expr&) -> stdx::option<const_value> { return stdx::none; },
        [&](const ast::address_of_expr&) -> stdx::option<const_value> { return stdx::none; },
        [&](ast::grouped_expr) { return try_eval(id); });
}

auto const_eval::eval_array(ast::node_id id, const ast::array_expr& array)
    -> stdx::option<const_value> {
    PROFILE_FUNCTION();
    // `[]T` / `[N]T` type expressions fold to the slice/array type the resolver assigned them.
    if (array.is_type_expr) {
        if (const auto sema_type{module_->get_sema_type_opt(id)}) {
            if (const auto meta{sema_type->get_data().as_opt<sema::types::meta_type>()}) {
                return const_value{meta->instance};
            }
            return const_value{*sema_type};
        }
        return stdx::none;
    }

    const_array arr;
    arr.elements.reserve(array.items.size());
    for (const auto& item_h : array.items) {
        const auto item_val{try_eval(item_h)};
        if (!item_val) { return stdx::none; }
        arr.elements.emplace_back(*item_val);
    }
    const auto sema_type{module_->get_sema_type_opt(id)};
    return const_value{std::move(arr), sema_type};
}

auto const_eval::eval_index(ast::node_id id, const ast::index_expr& index_expr)
    -> stdx::option<const_value> {
    PROFILE_FUNCTION();
    const auto target_val{try_eval(index_expr.array)};
    const auto idx_val{try_eval(index_expr.index)};
    if (!target_val || !idx_val) { return stdx::none; }

    const auto idx_opt{idx_val->as_int_opt()};
    if (!idx_opt || *idx_opt < 0) {
        ctx_.diags.emplace_back("Array index must be a non-negative integer",
                                sema::error::CONSTEXPR_EVALUATION_FAILED,
                                module_->ast.location_of(index_expr.index));
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
                module_->ast.location_of(id));
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
                module_->ast.location_of(id));
            return const_value::make_poison();
        }
        return const_value{static_cast<u64>(static_cast<u8>((*str)[idx])), ctx_.get_int(8, false)};
    }

    return stdx::none;
}

auto const_eval::eval_initializer(ast::node_id id, const ast::initializer_expr& init)
    -> stdx::option<const_value> {
    PROFILE_FUNCTION();
    const auto sema_type{module_->get_sema_type_opt(id)};

    if (sema_type) {
        const auto& type_data{sema_type->get_data()};

        // `RowAlias{ a, b, c }` folds to a constant array of the evaluated positional values.
        if (type_data.is<sema::types::array>()) {
            const_array arr;
            arr.elements.reserve(init.initializers.size());
            for (const auto& item : init.initializers) {
                const auto elem{try_eval(item.value)};
                if (!elem) { return stdx::none; }
                arr.elements.emplace_back(*elem);
            }
            return const_value{std::move(arr), sema_type};
        }

        const auto& table{ctx_.registry.get(sema_type->get_symbol_table_idx())};
        if (const auto st{type_data.as_opt<sema::types::struct_t>()}) {
            const_struct struct_val;
            for (const auto& item : init.initializers) {
                const auto& member_ident{
                    module_->ast.get_as<ast::implicit_access_expr>(*item.member)};
                const auto& member_name{
                    module_->ast.get_as<ast::identifier_expr>(member_ident.member).name};
                // A reference field needs an address to bind to
                if (const auto proxy{table.get_proxy_opt(member_name)}) {
                    if (st->type_at(proxy->index).get_kind() == sema::type_kind::REFERENCE) {
                        return stdx::none;
                    }
                }
                const auto field_val{try_eval(item.value)};
                if (!field_val) { return stdx::none; }
                struct_val.fields.emplace(std::string{member_name}, *field_val);
            }
            return const_value{std::move(struct_val), sema_type};
        }
        if (const auto ut{type_data.as_opt<sema::types::union_t>()}) {
            if (!init.initializers.empty()) {
                const auto& item{init.initializers.front()};
                const auto& member_ident{
                    module_->ast.get_as<ast::implicit_access_expr>(*item.member)};
                const auto& member_name{
                    module_->ast.get_as<ast::identifier_expr>(member_ident.member).name};
                if (const auto proxy{table.get_proxy_opt(member_name)}) {
                    if (ut->type_at(proxy->index).get_kind() == sema::type_kind::REFERENCE) {
                        return stdx::none;
                    }
                }
                const auto field_val{try_eval(item.value)};
                if (!field_val) { return stdx::none; }
                std::vector<const_value> payload;
                payload.emplace_back(*field_val);
                return const_value{const_union{std::string{member_name}, std::move(payload)},
                                   sema_type};
            }
        }
    }
    if (!sema_type) { return stdx::none; }

    const_struct struct_val;
    for (const auto& item : init.initializers) {
        if (!item.member) { return stdx::none; }
        const auto& member_ident{module_->ast.get_as<ast::implicit_access_expr>(*item.member)};
        const auto& member_name{
            module_->ast.get_as<ast::identifier_expr>(member_ident.member).name};
        const auto field_val{try_eval(item.value)};
        if (!field_val) { return stdx::none; }
        struct_val.fields.emplace(std::string{member_name}, *field_val);
    }
    return const_value{std::move(struct_val), sema_type};
}

auto const_eval::eval_dot(ast::node_id, const ast::dot_expr& dot) -> stdx::option<const_value> {
    PROFILE_FUNCTION();
    const auto& member_name{module_->ast.get_as<ast::identifier_expr>(dot.member).name};

    if (const auto obj_ident{module_->ast.get_as_opt<ast::identifier_expr>(dot.object)}) {
        if (module_->root_table_idx) {
            const auto& table{ctx_.registry.get(*module_->root_table_idx)};
            if (const auto sym{table.get_opt(obj_ident->name)}) {
                if (const auto node{sym->get_data().as_opt<sema::symbols::node_t>()}) {
                    if (const auto decl{module_->ast.get_as_opt<ast::decl_stmt>(*node)};
                        decl && decl->value) {
                        if (const auto en_expr{
                                module_->ast.get_as_opt<ast::enum_expr>(*decl->value)}) {
                            for (usize idx{0}; idx < en_expr->enumerations.size(); ++idx) {
                                const auto& e{en_expr->enumerations[idx]};
                                const auto& vname{
                                    module_->ast.get_as<ast::identifier_expr>(e.name).name};
                                if (vname == member_name) {
                                    i64 val{static_cast<i64>(idx)};
                                    if (e.value) {
                                        if (const auto ev{try_eval(*e.value)}) {
                                            val = static_cast<i64>(ev->as_int_opt().value_or(val));
                                        }
                                    }
                                    return const_value{const_enum{std::string{member_name}, val},
                                                       module_->get_sema_type_opt(*decl->value)};
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
            module_->ast.location_of(dot.member));
        return const_value::make_poison();
    }

    if (const auto type_opt{target_val->as_opt<stdx::option<sema::type&>>()};
        type_opt && *type_opt) {
        auto* denoted{&**type_opt};
        if (denoted->get_kind() == sema::type_kind::TYPE) {
            if (const auto meta{denoted->get_data().as_opt<sema::types::meta_type>()}) {
                denoted = &meta->instance;
            }
        }
        auto&      type{*denoted};
        const auto kind{type.get_kind()};

        // An enum variant reached through anything other than a bare identifier object (e.g.
        // `builtin::MemoryOrder.seq_cst`, where `dot.object` is a `module_access_expr`) misses
        // the bare-identifier fast path above; look it up the same way `eval_implicit_access`
        // does for a `.seq_cst`-style implicit access against a known enum type.
        if (const auto en{type.get_data().as_opt<sema::types::enum_t>()}) {
            for (usize idx{0}; idx < en->ast_enumerations.size(); ++idx) {
                const auto& e{en->ast_enumerations[idx]};
                const auto& vname{en->enclosing.ast.get_as<ast::identifier_expr>(e.name).name};
                if (vname == member_name) {
                    auto val{static_cast<i64>(idx)};
                    if (e.value) {
                        // The initializer expression is a node in the enum's defining module's
                        // AST arena, which is not necessarily the module currently being
                        // const-evaluated; evaluate it against the defining module.
                        auto&      enclosing_mod{const_cast<mod::module&>(en->enclosing)};
                        const_eval enclosing_eval{ctx_, enclosing_mod};
                        enclosing_eval.set_symbol_scoping(symbol_scoping_);
                        if (const auto ev{enclosing_eval.try_eval(*e.value)}) {
                            val = static_cast<i64>(ev->as_int_opt().value_or(val));
                        }
                    }
                    return const_value{const_enum{std::string{member_name}, val}, type};
                }
            }
        }

        const bool is_structural{kind == sema::type_kind::STRUCT ||
                                 kind == sema::type_kind::UNION || kind == sema::type_kind::ENUM};
        if (is_structural) {
            if (const auto tbl_idx{type.get_symbol_table_idx_opt()}) {
                const auto& mtable{ctx_.registry.get(*tbl_idx)};
                if (const auto msym{mtable.get_opt(member_name)}) {
                    if (const auto node{msym->get_data().as_opt<sema::symbols::node_t>()}) {
                        if (const auto mdecl{module_->ast.get_as_opt<ast::decl_stmt>(*node)};
                            mdecl && mdecl->value &&
                            (mdecl->has_modifier(ast::decl_modifiers::CONSTANT) ||
                             mdecl->has_modifier(ast::decl_modifiers::CONSTEXPR))) {
                            // A static member function decays to a value naming its GIR symbol
                            if (module_->ast.get_as_opt<ast::function_expr>(*mdecl->value)) {
                                if (const auto fn_type{module_->get_sema_type_opt(*mdecl->value)};
                                    fn_type && fn_type->get_kind() == sema::type_kind::FUNCTION) {
                                    return const_value{scoped_symbol_name(*tbl_idx, member_name),
                                                       *fn_type};
                                }
                                return stdx::none;
                            }
                            return try_eval(*mdecl->value);
                        }
                    }
                }
            }
        }
    }

    return stdx::none;
}

auto const_eval::target_enum_value(std::string_view enum_name, std::string_view member)
    -> const_value {
    PROFILE_FUNCTION();
    auto&      enum_type{ctx_.get_builtin_type(enum_name)};
    const auto en{enum_type.get_data().as_opt<sema::types::enum_t>()};

    // The discriminant must equal what `.<member>` produces against this enum type
    i64 ordinal{en ? static_cast<i64>(en->ast_enumerations.size()) : 0};
    if (en) {
        for (usize i{0}; i < en->ast_enumerations.size(); ++i) {
            const auto& vname{
                en->enclosing.ast.get_as<ast::identifier_expr>(en->ast_enumerations[i].name).name};
            if (vname == member) {
                ordinal = static_cast<i64>(i);
                break;
            }
        }
    }
    return const_value{const_enum{std::string{member}, ordinal}, enum_type};
}

auto const_eval::eval_implicit_access(ast::node_id id, const ast::implicit_access_expr& implicit)
    -> stdx::option<const_value> {
    PROFILE_FUNCTION();
    const auto& member_name{module_->ast.get_as<ast::identifier_expr>(implicit.member).name};
    const auto  sema_type{module_->get_sema_type_opt(id)};

    if (sema_type) {
        if (const auto en{sema_type->get_data().as_opt<sema::types::enum_t>()}) {
            for (usize idx{0}; idx < en->ast_enumerations.size(); ++idx) {
                const auto& e{en->ast_enumerations[idx]};
                // Variant identifiers live in the enum's defining module, which is not
                // necessarily the module being const-evaluated.
                const auto& vname{en->enclosing.ast.get_as<ast::identifier_expr>(e.name).name};
                if (vname == member_name) {
                    auto val{static_cast<i64>(idx)};
                    if (e.value) {
                        // As in eval_dot: the initializer lives in the enum's defining module's
                        // AST arena, which may differ from the module currently being evaluated.
                        auto&      enclosing_mod{const_cast<mod::module&>(en->enclosing)};
                        const_eval enclosing_eval{ctx_, enclosing_mod};
                        enclosing_eval.set_symbol_scoping(symbol_scoping_);
                        if (const auto ev{enclosing_eval.try_eval(*e.value)}) {
                            val = static_cast<i64>(ev->as_int_opt().value_or(val));
                        }
                    }
                    return const_value{const_enum{std::string{member_name}, val}, sema_type};
                }
            }
        }
    }

    // Not an enum member so let the caller fall back
    return stdx::none;
}

auto const_eval::resolve_module_chain(ast::node_id node) -> stdx::option<mod::module&> {
    PROFILE_FUNCTION();
    // Base case: a bare identifier naming an imported module in the current scope.
    if (const auto ident{module_->ast.get_as_opt<ast::identifier_expr>(node)}) {
        if (!module_->root_table_idx) { return stdx::none; }
        const auto& table{ctx_.registry.get(*module_->root_table_idx)};
        const auto  sym{table.get_opt(ident->name)};
        if (!sym) { return stdx::none; }
        const auto kind{sym->get_kind_opt()};
        if (!kind || *kind != sema::symbol_kind::MODULE) { return stdx::none; }
        const auto snode{sym->get_data().as_opt<sema::symbols::node_t>()};
        if (!snode) { return stdx::none; }
        const auto sema_type{module_->get_sema_type_opt(*snode)};
        if (!sema_type) { return stdx::none; }
        const auto m_data{sema_type->get_data().as_opt<sema::types::module>()};
        if (!m_data) { return stdx::none; }
        return m_data->imported;
    }

    // Recursive case: `<outer>::<segment>` where `<outer>` itself names a module and
    // `<segment>` is a module re-exported from it.
    if (const auto nested{module_->ast.get_as_opt<ast::module_access_expr>(node)}) {
        const auto outer_mod{resolve_module_chain(nested->outer)};
        if (!outer_mod || !outer_mod->root_table_idx) { return stdx::none; }
        const auto& seg_name{module_->ast.get_as<ast::identifier_expr>(nested->inner).name};
        const auto& table{ctx_.registry.get(*outer_mod->root_table_idx)};
        const auto  sym{table.get_opt(seg_name)};
        if (!sym) { return stdx::none; }
        const auto kind{sym->get_kind_opt()};
        if (!kind || *kind != sema::symbol_kind::MODULE) { return stdx::none; }
        const auto snode{sym->get_data().as_opt<sema::symbols::node_t>()};
        if (!snode) { return stdx::none; }
        const auto sema_type{outer_mod->get_sema_type_opt(*snode)};
        if (!sema_type) { return stdx::none; }
        const auto m_data{sema_type->get_data().as_opt<sema::types::module>()};
        if (!m_data) { return stdx::none; }
        return m_data->imported;
    }

    return stdx::none;
}

auto const_eval::eval_module_member(mod::module& target_mod, std::string_view member)
    -> stdx::option<const_value> {
    PROFILE_FUNCTION();
    if (!target_mod.root_table_idx) { return stdx::none; }
    const auto& table{ctx_.registry.get(*target_mod.root_table_idx)};

    const auto sym{table.get_opt(member)};
    if (!sym) { return stdx::none; }
    const auto node{sym->get_data().as_opt<sema::symbols::node_t>()};
    if (!node) { return stdx::none; }

    if (const auto using_stmt{target_mod.ast.get_as_opt<ast::using_stmt>(*node)}) {
        // Yield the aliased type so `mod::Alias.MEMBER` can resolve through it
        if (const auto aliased{target_mod.get_sema_type_opt(using_stmt->explicit_type)}) {
            return const_value{*aliased};
        }
        return stdx::none;
    }

    const auto decl{target_mod.ast.get_as_opt<ast::decl_stmt>(*node)};
    if (!decl || !decl->value) { return stdx::none; }

    // A plain cross-module function decays to a value naming its GIR symbol
    if (target_mod.ast.get_as_opt<ast::function_expr>(*decl->value)) {
        const auto fn_type{target_mod.get_sema_type_opt(*decl->value)};
        if (fn_type && fn_type->get_kind() == sema::type_kind::FUNCTION) {
            return const_value{scoped_symbol_name(*target_mod.root_table_idx, member), *fn_type};
        }
        return stdx::none;
    }

    const_eval inner_eval{ctx_, target_mod};
    inner_eval.set_symbol_scoping(symbol_scoping_);
    return inner_eval.try_eval(*decl->value);
}

auto const_eval::eval_module_access(ast::node_id, const ast::module_access_expr& mod_access)
    -> stdx::option<const_value> {
    PROFILE_FUNCTION();
    const auto& inner_name{module_->ast.get_as<ast::identifier_expr>(mod_access.inner).name};

    // Resolve the outer prefix to a module, then read inner from it
    if (module_->ast.get_as_opt<ast::module_access_expr>(mod_access.outer)) {
        const auto target_mod{resolve_module_chain(mod_access.outer)};
        if (!target_mod) { return stdx::none; }
        return eval_module_member(*target_mod, inner_name);
    }

    const auto outer_ident{module_->ast.get_as_opt<ast::identifier_expr>(mod_access.outer)};
    if (!outer_ident || !module_->root_table_idx) { return stdx::none; }
    const auto& table{ctx_.registry.get(*module_->root_table_idx)};

    // `builtin::X` (and any other prelude-injected module) lives in the prelude table, not the
    // resolving module's own root table -- fall back to it the same way the resolver's
    // scope-chain lookup (`ctx_.registry.lookup(table_stack_, ...)`) would.
    auto sym{table.get_opt(outer_ident->name)};
    if (!sym && ctx_.prelude_index) {
        sym = ctx_.registry.get(*ctx_.prelude_index).get_opt(outer_ident->name);
    }
    if (!sym) { return stdx::none; }

    // A prelude module (like `builtin`) is a `symbols::builtin` entry carrying its type
    // directly, not an AST-backed `symbols::node_t`; check MODULE-kind first so both shapes
    // reach the module lookup below.
    if (const auto node_sym{sym->get_kind_opt()};
        node_sym && *node_sym == sema::symbol_kind::MODULE) {
        stdx::option<sema::type&> sema_type;
        if (const auto bi{sym->get_data().as_opt<sema::symbols::builtin>()}) {
            sema_type.emplace(bi->get_type());
        } else if (const auto node{sym->get_data().as_opt<sema::symbols::node_t>()}) {
            sema_type = module_->get_sema_type_opt(*node);
        }
        if (!sema_type) { return stdx::none; }
        const auto m_data{sema_type->get_data().as_opt<sema::types::module>()};
        if (!m_data) { return stdx::none; }
        return eval_module_member(m_data->imported, inner_name);
    }

    const auto node{sym->get_data().as_opt<sema::symbols::node_t>()};
    if (!node) { return stdx::none; }

    if (const auto decl{module_->ast.get_as_opt<ast::decl_stmt>(*node)}) {
        if (!decl->value) { return stdx::none; }
        const auto en_expr{module_->ast.get_as_opt<ast::enum_expr>(*decl->value)};
        if (!en_expr) { return stdx::none; }

        for (usize idx{0}; idx < en_expr->enumerations.size(); ++idx) {
            const auto& e{en_expr->enumerations[idx]};
            const auto& vname{module_->ast.get_as<ast::identifier_expr>(e.name).name};
            if (vname == inner_name) {
                i64 val{static_cast<i64>(idx)};
                if (e.value) {
                    if (const auto ev{try_eval(*e.value)}) {
                        val = static_cast<i64>(ev->as_int_opt().value_or(val));
                    }
                }
                return const_value{const_enum{std::string{inner_name}, val},
                                   module_->get_sema_type_opt(*decl->value)};
            }
        }
    }

    return stdx::none;
}

auto const_eval::eval_match(ast::node_id id, const ast::match_expr& match)
    -> stdx::option<const_value> {
    PROFILE_FUNCTION();
    const auto matcher_val{try_eval(match.matcher)};
    if (!matcher_val) { return stdx::none; }

    const auto eval_dispatch = [&](const ast::stmt_handle& dispatch) -> stdx::option<const_value> {
        return module_->ast[*dispatch].visit(
            [&](const auto&) -> stdx::option<const_value> { return eval_stmt(dispatch); },
            [&](const ast::expr_stmt& data) -> stdx::option<const_value> {
                return try_eval(data.expression);
            });
    };

    for (const auto& arm : match.arms) {
        const bool arm_matches{std::ranges::any_of(arm.patterns, [&](const auto& pattern) {
            return match_pattern(pattern, *matcher_val);
        })};
        if (arm_matches) {
            if (arm.capture && arm.capture->template is<ast::identifier_expr>()) {
                const auto& ident{module_->ast.get_as<ast::identifier_expr>(*arm.capture)};
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
                            module_->ast.location_of(id));
    return const_value::make_poison();
}

auto const_eval::eval_unwrap(ast::node_id id, const ast::unwrap_expr& unwrap)
    -> stdx::option<const_value> {
    PROFILE_FUNCTION();
    const auto operand{try_eval(unwrap.operand)};
    if (!operand) { return stdx::none; }

    const auto un{operand->as_opt<const_union>()};
    if (!un) { return stdx::none; }

    const auto payload{
        [&] -> const_value { return un->payload.empty() ? *operand : un->payload.front(); }};
    if (un->active_field == "ok" || un->active_field == "some") { return payload(); }

    if (id.get_token_type() == syntax::token_type_t::BANG) {
        ctx_.diags.emplace_back(
            fmt::format("compile-time '!' on a union whose active field is '{}'", un->active_field),
            sema::error::CONSTEXPR_EVALUATION_FAILED,
            module_->ast.location_of(id));
        return const_value::make_poison();
    }

    return payload();
}

auto const_eval::match_pattern(const ast::match_pattern_handle& pattern_h,
                               const const_value&               target) -> bool {
    PROFILE_FUNCTION();
    const auto pattern_id{*pattern_h};

    if (pattern_h.is<ast::discarded>()) { return true; }

    if (pattern_h.is<ast::implicit_access_expr>()) {
        const auto& imp{module_->ast.get_as<ast::implicit_access_expr>(pattern_id)};
        const auto& name{module_->ast.get_as<ast::identifier_expr>(imp.member).name};
        if (const auto en{target.as_opt<const_enum>()}) { return en->name == name; }
        if (const auto un{target.as_opt<const_union>()}) { return un->active_field == name; }
        return false;
    }

    if (const auto dot{module_->ast.get_as_opt<ast::dot_expr>(pattern_id)}) {
        const auto& member_name{module_->ast.get_as<ast::identifier_expr>(dot->member).name};
        if (const auto en{target.as_opt<const_enum>()}) { return en->name == member_name; }
        if (const auto un{target.as_opt<const_union>()}) { return un->active_field == member_name; }
    }

    if (const auto range{module_->ast.get_as_opt<ast::range_expr>(pattern_id)}) {
        if (!range->lhs || !range->rhs) { return false; }
        const auto start_val{try_eval(*range->lhs)};
        const auto end_val{try_eval(*range->rhs)};
        if (start_val && end_val) {
            const auto target_int{target.as_int_opt()};
            const auto start_int{start_val->as_int_opt()};
            const auto end_int{end_val->as_int_opt()};
            if (target_int && start_int && end_int) {
                const bool inclusive{pattern_id.get_token_type() ==
                                     syntax::token_type_t::DOT_DOT_EQ};
                return *target_int >= *start_int &&
                       (inclusive ? *target_int <= *end_int : *target_int < *end_int);
            }
        }
    }

    if (const auto pat_val{try_eval(pattern_id)}) { return *pat_val == target; }

    return false;
}

auto const_eval::eval_assignment(ast::node_id                id,
                                 const ast::assignment_expr& assign,
                                 syntax::token_type_t        op_type) -> stdx::option<const_value> {
    PROFILE_FUNCTION();
    const auto ident{module_->ast.get_as_opt<ast::identifier_expr>(assign.lhs)};
    if (!ident) { return stdx::none; }

    if (op_type == syntax::token_type_t::ASSIGN) {
        const auto rhs{try_eval(assign.rhs)};
        if (!rhs) { return stdx::none; }
        if (!set_local_binding(ident->name, *rhs)) { return stdx::none; }
        return rhs;
    }

    if (const auto base_op{syntax::token_type::get_compound_base_op(op_type)}) {
        const auto lhs_val{try_eval(assign.lhs)};
        const auto rhs_val{try_eval(assign.rhs)};
        if (!lhs_val || !rhs_val) { return stdx::none; }

        const auto folded{fold_binary_values(*base_op, *lhs_val, *rhs_val, id)};
        if (!folded) { return stdx::none; }

        if (!set_local_binding(ident->name, *folded)) { return stdx::none; }
        return folded;
    }

    return stdx::none;
}

auto const_eval::fold_binary_values(syntax::token_type_t op_type,
                                    const const_value&   lhs,
                                    const const_value&   rhs,
                                    ast::node_id         id) -> stdx::option<const_value> {
    PROFILE_FUNCTION();

    // Wrapping ops fold as their plain base op, then truncate to the operand's concrete width
    // (two's-complement wrap). A width-less `constexpr_int` result has nothing to wrap to, so it
    // folds exactly as the plain operator would.
    if (const auto plain_op{wrapping_base_op(op_type)}) {
        const auto folded{fold_binary_values(*plain_op, lhs, rhs, id)};
        if (!folded || folded->is_poison()) { return folded; }
        const auto res_type{folded->get_type()};
        if (!res_type) { return folded; }
        if (res_type->get_kind() == sema::type_kind::INT) {
            return wrap_to_width(
                *folded, sema::int_width(*res_type), sema::is_signed_integer(*res_type), res_type);
        }
        if (res_type->get_kind() == sema::type_kind::ISIZE ||
            res_type->get_kind() == sema::type_kind::USIZE) {
            const auto ptr_bits{
                codegen::target_facts::resolve(ctx_.target_opts.triple_str).ptr_bits};
            return wrap_to_width(*folded,
                                 static_cast<u16>(ptr_bits),
                                 res_type->get_kind() == sema::type_kind::ISIZE,
                                 res_type);
        }
        // `constexpr_int` (or anything else non-scalar-width): no wrap, plain-op result stands.
        return folded;
    }

    auto& bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};

    const auto on_div_zero = [&](std::string_view msg) -> const_value {
        ctx_.diags.emplace_back(std::string{msg},
                                sema::error::CONSTEXPR_EVALUATION_FAILED,
                                module_->ast.location_of(id));
        return const_value::make_poison();
    };

    const auto is_wide_arm    = [](const const_value& v) { return v.is<i128>() || v.is<u128>(); };
    const auto is_narrow_int  = [](const const_value& v) { return v.is<i64>() || v.is<u64>(); };
    const auto is_signed_wide = [](const const_value& v) { return v.is<i64>() || v.is<i128>(); };

    if (lhs.is<f64>() || rhs.is<f64>()) {
        const auto to_f64 = [](const const_value& v) -> f64 {
            if (v.is<f64>()) { return v.as<f64>(); }
            if (const auto u{v.as_uint_opt()}) { return static_cast<f64>(*u); }
            return static_cast<f64>(v.as_int_opt().value_or(0));
        };
        const auto res_type{lhs.get_type() ? lhs.get_type() : rhs.get_type()};
        // `f80`/`f128` results cannot be represented exactly at compile time; refuse to fold (D3).
        if (res_type && (res_type->get_kind() == sema::type_kind::F80 ||
                         res_type->get_kind() == sema::type_kind::F128)) {
            return stdx::none;
        }
        return fold_binary_arithmetic(
            op_type, to_f64(lhs), to_f64(rhs), res_type, bool_type, on_div_zero);
    }

    // A wide (128-bit) operand pulls the whole operation into the 128-bit comptime domain.
    if ((is_wide_arm(lhs) || is_wide_arm(rhs)) && (is_wide_arm(lhs) || is_narrow_int(lhs)) &&
        (is_wide_arm(rhs) || is_narrow_int(rhs))) {
        const auto res_type{lhs.get_type() ? lhs.get_type() : rhs.get_type()};
        const bool unsigned_op{
            (lhs.is<u64>() || lhs.is<u128>() || rhs.is<u64>() || rhs.is<u128>()) &&
            !is_signed_wide(lhs)};
        if (unsigned_op) {
            return fold_binary_arithmetic(op_type,
                                          lhs.as_uint_opt().value_or(0),
                                          rhs.as_uint_opt().value_or(0),
                                          res_type,
                                          bool_type,
                                          on_div_zero);
        }
        return fold_binary_arithmetic(op_type,
                                      lhs.as_int_opt().value_or(0),
                                      rhs.as_int_opt().value_or(0),
                                      res_type,
                                      bool_type,
                                      on_div_zero);
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

    if (lhs.is<const_enum>() && rhs.is<const_enum>()) {
        const auto equal{lhs.as<const_enum>() == rhs.as<const_enum>()};
        if (op_type == syntax::token_type_t::EQ) { return const_value{equal, bool_type}; }
        if (op_type == syntax::token_type_t::NEQ) { return const_value{!equal, bool_type}; }
    }

    // Type-valued operands: `@typeOf(x) == u8`, `T != i32`, ... in an `if constexpr`.
    if (lhs.is<stdx::option<sema::type&>>() && rhs.is<stdx::option<sema::type&>>()) {
        const auto l{lhs.as<stdx::option<sema::type&>>()};
        const auto r{rhs.as<stdx::option<sema::type&>>()};
        if (l && r) {
            const bool equal{sema::is_same_unqualified(*l, *r)};
            if (op_type == syntax::token_type_t::EQ) { return const_value{equal, bool_type}; }
            if (op_type == syntax::token_type_t::NEQ) { return const_value{!equal, bool_type}; }
        }
    }

    if (lhs.is<std::string>() && rhs.is<std::string>()) {
        const auto& l{lhs.as<std::string>()};
        const auto& r{rhs.as<std::string>()};
        if (op_type == syntax::token_type_t::PLUS) { return const_value::make_string(ctx_, l + r); }
        if (op_type == syntax::token_type_t::EQ) { return const_value{l == r, bool_type}; }
        if (op_type == syntax::token_type_t::NEQ) { return const_value{l != r, bool_type}; }
        if (op_type == syntax::token_type_t::LT) { return const_value{l < r, bool_type}; }
        if (op_type == syntax::token_type_t::LT_EQ) { return const_value{l <= r, bool_type}; }
        if (op_type == syntax::token_type_t::GT) { return const_value{l > r, bool_type}; }
        if (op_type == syntax::token_type_t::GT_EQ) { return const_value{l >= r, bool_type}; }
    }

    return stdx::none;
}

auto const_eval::eval_binary(ast::node_id id, const ast::binary_expr& binary)
    -> stdx::option<const_value> {
    PROFILE_FUNCTION();
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
    PROFILE_FUNCTION();
    const auto val{try_eval(unary.rhs)};
    if (!val) { return stdx::none; }

    const auto op_type{id.get_token_type()};
    if (op_type == syntax::token_type_t::MINUS) {
        if (val->is<i64>()) { return const_value{-val->as<i64>(), val->get_type()}; }
        if (val->is<u64>()) {
            return const_value{-static_cast<i64>(val->as<u64>()), val->get_type()};
        }
        if (val->is<f64>()) { return const_value{-val->as<f64>(), val->get_type()}; }
    } else if (op_type == syntax::token_type_t::MINUS_PERCENT) {
        // '-%' is restricted to signed integers by sema, so only signed arms meaningful here
        stdx::option<const_value> negated;
        if (val->is<i64>()) { negated.emplace(-val->as<i64>(), val->get_type()); }
        if (val->is<i128>()) { negated.emplace(-val->as<i128>(), val->get_type()); }
        if (!negated) { return stdx::none; }

        const auto res_type{negated->get_type()};
        if (res_type && res_type->get_kind() == sema::type_kind::INT) {
            return wrap_to_width(*negated, sema::int_width(*res_type), true, res_type);
        }
        if (res_type && res_type->get_kind() == sema::type_kind::ISIZE) {
            const auto ptr_bits{
                codegen::target_facts::resolve(ctx_.target_opts.triple_str).ptr_bits};
            return wrap_to_width(*negated, static_cast<u16>(ptr_bits), true, res_type);
        }
        return negated; // `constexpr_int`: no wrap, plain negate stands.
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
    PROFILE_FUNCTION();
    if (auto local_val{lookup_local_binding(ident.name)}) { return local_val; }
    if (const auto cx{ctx_.lookup_constexpr_binding(ident.name)}) { return *cx; }

    // `iN` / `uN` name a primitive integer type; yield it as a compile-time type value.
    if (syntax::token_type::is_int_type_lexeme(ident.name)) {
        u64 width{0};
        for (const char c : stdx::string::substr(ident.name, 1)) {
            width = width * 10 + static_cast<u64>(c - '0');
            if (width > 65'535) { break; }
        }
        if (width >= 1 && width <= 65'535) {
            return const_value{ctx_.get_int(static_cast<u16>(width), ident.name.front() == 'i')};
        }
    }

    // A bare identifier inside a member body may name a sibling static `const` member.
    if (enclosing_type_) {
        if (const auto tbl{enclosing_type_->get_symbol_table_idx_opt()}) {
            if (const auto msym{ctx_.registry.get_from_opt(*tbl, ident.name)}) {
                if (const auto node{msym->get_data().as_opt<sema::symbols::node_t>()}) {
                    if (const auto mdecl{module_->ast.get_as_opt<ast::decl_stmt>(*node)};
                        mdecl && mdecl->value &&
                        (mdecl->has_modifier(ast::decl_modifiers::CONSTANT) ||
                         mdecl->has_modifier(ast::decl_modifiers::CONSTEXPR)) &&
                        !module_->ast.get_as_opt<ast::function_expr>(*mdecl->value)) {
                        return try_eval(*mdecl->value);
                    }
                }
            }
        }
    }

    if (id.is_valid()) {
        if (const auto sema_type{module_->get_sema_type_opt(id)}) {
            if (sema_type->get_kind() == sema::type_kind::TYPE) {
                if (const auto meta{sema_type->get_data().as_opt<sema::types::meta_type>()}) {
                    return const_value{meta->instance};
                }
                return const_value{*sema_type};
            }
        }
    }

    if (!module_->root_table_idx) { return stdx::none; }
    const auto& table{ctx_.registry.get(*module_->root_table_idx)};
    const auto  sym_opt{table.get_opt(ident.name)};
    if (!sym_opt) {
        if (ctx_.prelude_index) {
            const auto& prelude{ctx_.registry.get(*ctx_.prelude_index)};
            if (const auto p_sym{prelude.get_opt(ident.name)}) {
                if (p_sym->has_kind() && p_sym->get_kind() == sema::symbol_kind::TYPE) {
                    if (const auto builtin{p_sym->get_data().as_opt<sema::symbols::builtin>()}) {
                        return const_value{builtin->get_type()};
                    }
                }
            }
        }
        return stdx::none;
    }
    const auto& sym{*sym_opt};

    if (sym.has_kind() && sym.get_kind() == sema::symbol_kind::TYPE) {
        if (const auto builtin_sym{sym.get_data().as_opt<sema::symbols::builtin>()}) {
            return const_value{builtin_sym->get_type()};
        }
        if (const auto node{sym.get_data().as_opt<sema::symbols::node_t>()}) {
            if (const auto using_stmt{module_->ast.get_as_opt<ast::using_stmt>(*node)}) {
                if (const auto sema_type{module_->get_sema_type_opt(using_stmt->explicit_type)}) {
                    return const_value{*sema_type};
                }
            }
            if (const auto decl{module_->ast.get_as_opt<ast::decl_stmt>(*node)}) {
                if (decl->value) {
                    const auto& node_data{module_->ast[*decl->value]};
                    if (node_data.is<ast::struct_expr>() || node_data.is<ast::enum_expr>() ||
                        node_data.is<ast::union_expr>()) {
                        if (const auto sema_type{module_->get_sema_type_opt(*decl->value)}) {
                            return const_value{*sema_type};
                        }
                    }
                    return try_eval(*decl->value);
                }
            }
        }
    }

    if (const auto node{sym.get_data().as_opt<sema::symbols::node_t>()}) {
        if (const auto decl{module_->ast.get_as_opt<ast::decl_stmt>(*node)}) {
            // A plain top-level function decays to a value referencing its own
            if (decl->value && module_->ast.get_as_opt<ast::function_expr>(*decl->value)) {
                if (const auto fn_type{module_->get_sema_type_opt(*decl->value)};
                    fn_type && fn_type->get_kind() == sema::type_kind::FUNCTION) {
                    return const_value{scoped_symbol_name(*module_->root_table_idx, ident.name),
                                       *fn_type};
                }
            }
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
    PROFILE_FUNCTION();
    const auto fn_token{call.function->get_token_type()};
    if (syntax::get_builtin_opt(fn_token)) { return eval_builtin(id, call, fn_token); }

    // A `constexpr` callable (closure or function) parameter invoked inside a monomorphized body.
    if (const auto ident{module_->ast.get_as_opt<ast::identifier_expr>(call.function)}) {
        if (const auto bc{lookup_bound_callable(ident->name)}) {
            std::vector<const_value> args;
            for (const auto& arg : call.arguments) {
                if (const auto expr_h{arg.as_opt<ast::expr_handle>()}) {
                    const auto av{try_eval(*expr_h)};
                    if (!av) { return stdx::none; }
                    args.emplace_back(*av);
                }
            }
            auto* const prev{module_.get()};
            set_module(*bc->module);
            auto res{eval_constexpr_fn(id, *bc->fn_expr, args, bc->captures)};
            set_module(*prev);
            return res;
        }
    }

    // Resolve `Ctor(...)` (same module) or `mod::Ctor(...)` (imported module) to (module, decl).
    stdx::option<mod::module&>  callee_mod;
    stdx::option<sema::symbol&> callee_sym;
    if (const auto ident{module_->ast.get_as_opt<ast::identifier_expr>(call.function)}) {
        if (module_->root_table_idx) {
            callee_mod.emplace(*module_);
            callee_sym = ctx_.registry.get_from_opt(*module_->root_table_idx, ident->name);
        }
    } else if (const auto mac{module_->ast.get_as_opt<ast::module_access_expr>(call.function)}) {
        if (const auto mod_ty{module_->get_sema_type_opt(mac->outer)}) {
            if (const auto m_data{mod_ty->get_data().as_opt<sema::types::module>()}) {
                if (m_data->imported.root_table_idx) {
                    callee_mod.emplace(m_data->imported);
                    const auto& inner{module_->ast.get_as<ast::identifier_expr>(mac->inner)};
                    callee_sym =
                        ctx_.registry.get_from_opt(*callee_mod->root_table_idx, inner.name);
                }
            }
        }
    }

    if (callee_mod && callee_sym) {
        const auto node{callee_sym->get_data().as_opt<sema::symbols::node_t>()};
        const auto decl{node ? callee_mod->ast.get_as_opt<ast::decl_stmt>(*node) : stdx::none};
        if (decl && decl->value &&
            (decl->has_modifier(ast::decl_modifiers::CONSTEXPR) ||
             decl->has_modifier(ast::decl_modifiers::CONSTANT))) {
            if (const auto fn_expr{callee_mod->ast.get_as_opt<ast::function_expr>(*decl->value)}) {
                std::vector<const_value> args;
                for (const auto& arg : call.arguments) {
                    if (const auto expr_h{arg.as_opt<ast::expr_handle>()}) {
                        const auto arg_val{try_eval(*expr_h)};
                        if (!arg_val) { return stdx::none; }
                        args.emplace_back(*arg_val);
                    }
                }
                auto* const prev{module_.get()};
                if (callee_mod != prev) { set_module(*callee_mod); }
                auto res{eval_constexpr_fn(id, *fn_expr, args)};
                if (callee_mod != prev) { set_module(*prev); }
                return res;
            }
        }
    }

    return stdx::none;
}

auto const_eval::eval_builtin(ast::node_id          id,
                              const ast::call_expr& call,
                              syntax::token_type_t  builtin_type) -> stdx::option<const_value> {
    PROFILE_FUNCTION();
    auto& usize_type{ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};

    switch (builtin_type) {
    case syntax::token_type_t::BUILTIN_THIS: {
        // The call node carries the enclosing structural type
        if (id.is_valid()) {
            if (const auto here{module_->get_sema_type_opt(id)}) { return const_value{here}; }
        }
        if (const auto target_type{module_->get_sema_type_opt(call.function)}) {
            return const_value{target_type};
        }
        return stdx::none;
    }
    case syntax::token_type_t::BUILTIN_SIZE_OF: {
        VERIFY(!call.arguments.empty(), "Arity mismatch not verified during resolution");
        const auto&               arg{call.arguments.front()};
        stdx::option<sema::type&> target_type;
        if (const auto type_id{arg.as_opt<ast::explicit_type_id>()}) {
            target_type = module_->get_sema_type_opt(*type_id);
        } else if (const auto expr_h{arg.as_opt<ast::expr_handle>()}) {
            target_type = module_->get_sema_type_opt(*expr_h);
        }
        if (!target_type) { return stdx::none; }

        if (const auto dc{target_type->get_data().as_opt<sema::types::deferred_call>()}) {
            if (const auto r{try_resolve_deferred_call(dc->call)}) { target_type.emplace(*r); }
        }

        const auto ptr_size{
            codegen::resolve_target_triple(ctx_.target_opts.triple_str).isArch64Bit() ? usize{8}
                                                                                      : usize{4}};
        if (const auto def{target_type->get_data().as_opt<sema::types::deferred_array>()}) {
            if (def->array.dimension) {
                const auto dim_opt{eval_type_dim(*def->array.dimension)};
                const auto len{dim_opt.value_or(0)};
                const auto elem_size{type_size_of(def->underlying, ptr_size)};
                const auto elem_align{type_align_of(def->underlying, ptr_size)};
                const auto elem_stride{elem_align > 0
                                           ? (elem_size + elem_align - 1) / elem_align * elem_align
                                           : elem_size};
                const auto total_sz{len * (elem_stride == 0 ? elem_size : elem_stride)};
                return const_value{static_cast<u64>(total_sz), usize_type};
            }
        }
        const auto sz{type_size_of(*target_type, ptr_size)};
        return const_value{static_cast<u64>(sz), usize_type};
    }
    case syntax::token_type_t::BUILTIN_ALIGN_OF: {
        VERIFY(!call.arguments.empty(), "Arity mismatch not verified during resolution");
        const auto&               arg{call.arguments.front()};
        stdx::option<sema::type&> target_type;
        if (const auto type_id = arg.as_opt<ast::explicit_type_id>()) {
            target_type = module_->get_sema_type_opt(*type_id);
        } else if (const auto expr_h = arg.as_opt<ast::expr_handle>()) {
            target_type = module_->get_sema_type_opt(*expr_h);
        }
        if (!target_type) { return stdx::none; }

        if (const auto dc{target_type->get_data().as_opt<sema::types::deferred_call>()}) {
            if (const auto r{try_resolve_deferred_call(dc->call)}) { target_type.emplace(*r); }
        }

        const auto ptr_size{
            codegen::resolve_target_triple(ctx_.target_opts.triple_str).isArch64Bit() ? usize{8}
                                                                                      : usize{4}};
        if (const auto def{target_type->get_data().as_opt<sema::types::deferred_array>()}) {
            return const_value{static_cast<u64>(type_align_of(def->underlying, ptr_size)),
                               usize_type};
        }
        const auto al{type_align_of(*target_type, ptr_size)};
        return const_value{static_cast<u64>(al), usize_type};
    }
    case syntax::token_type_t::BUILTIN_BIT_SIZE_OF: {
        VERIFY(!call.arguments.empty(), "Arity mismatch not verified during resolution");
        const auto&               arg{call.arguments.front()};
        stdx::option<sema::type&> target_type;
        if (const auto type_id{arg.as_opt<ast::explicit_type_id>()}) {
            target_type = module_->get_sema_type_opt(*type_id);
        } else if (const auto expr_h{arg.as_opt<ast::expr_handle>()}) {
            target_type = module_->get_sema_type_opt(*expr_h);
        }
        if (!target_type) { return stdx::none; }

        if (const auto dc{target_type->get_data().as_opt<sema::types::deferred_call>()}) {
            if (const auto r{try_resolve_deferred_call(dc->call)}) { target_type.emplace(*r); }
        }
        // `@bitSizeOf(@typeOf(x))` hands us a `meta_type`; unwrap to the denoted type.
        if (target_type->get_kind() == sema::type_kind::TYPE) {
            if (const auto m{target_type->get_data().as_opt<sema::types::meta_type>()}) {
                target_type.emplace(m->instance);
            }
        }

        const auto ptr_size{
            codegen::resolve_target_triple(ctx_.target_opts.triple_str).isArch64Bit() ? usize{8}
                                                                                      : usize{4}};
        const auto ptr_bits{static_cast<u32>(ptr_size) * 8};

        const auto kind{target_type->get_kind()};
        if (kind == sema::type_kind::INT) {
            return const_value{static_cast<u64>(sema::int_width(*target_type)), usize_type};
        }
        if (kind == sema::type_kind::ISIZE || kind == sema::type_kind::USIZE) {
            return const_value{static_cast<u64>(ptr_bits), usize_type};
        }
        if (kind == sema::type_kind::BOOL) { return const_value{u64{1}, usize_type}; }
        if (sema::is_float(kind)) {
            return const_value{static_cast<u64>(sema::float_bits(kind)), usize_type};
        }
        if (const auto st{target_type->get_data().as_opt<sema::types::struct_t>()};
            st && st->is_bit_packed()) {
            if (const auto n{sema::packed_backing_bits(*st, ptr_bits)}) {
                return const_value{static_cast<u64>(*n), usize_type};
            }
        }
        if (const auto ut{target_type->get_data().as_opt<sema::types::union_t>()};
            ut && ut->is_bit_packed()) {
            if (const auto n{sema::packed_union_backing_bits(*ut, ptr_bits)}) {
                return const_value{static_cast<u64>(*n), usize_type};
            }
        }
        return const_value{static_cast<u64>(type_size_of(*target_type, ptr_size) * 8), usize_type};
    }
    case syntax::token_type_t::BUILTIN_TYPE_OF: {
        VERIFY(!call.arguments.empty(), "Arity mismatch not verified during resolution");
        const auto& arg{call.arguments.front()};
        if (const auto expr_h{arg.as_opt<ast::expr_handle>()}) {
            const auto target_type{module_->get_sema_type_opt(*expr_h)};
            if (target_type) { return const_value{target_type}; }
        }
        return stdx::none;
    }
    case syntax::token_type_t::BUILTIN_IMPLEMENTS: {
        VERIFY(call.arguments.size() == 2, "Arity mismatch not verified during resolution");
        const auto arg_type{[&](const ast::call_expr::argument& a) -> stdx::option<sema::type&> {
            if (const auto tid{a.as_opt<ast::explicit_type_id>()}) {
                return module_->get_sema_type_opt(*tid);
            }
            if (const auto eh{a.as_opt<ast::expr_handle>()}) {
                return module_->get_sema_type_opt(*eh);
            }
            return stdx::none;
        }};

        const auto denote{[](sema::type& t) -> sema::type& {
            if (t.get_kind() == sema::type_kind::TYPE) {
                if (const auto m{t.get_data().as_opt<sema::types::meta_type>()}) {
                    return m->instance;
                }
            }
            return t;
        }};

        auto t0{arg_type(call.arguments[0])};
        auto t1{arg_type(call.arguments[1])};
        if (!t0 || !t1) { return stdx::none; }

        auto& target{denote(*t0)};
        auto& iface{denote(*t1)};
        auto& bool_type{ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
        if (iface.get_kind() != sema::type_kind::INTERFACE) {
            return const_value{false, bool_type};
        }
        return const_value{ctx_.impls.implements(target, iface), bool_type};
    }
    case syntax::token_type_t::BUILTIN_ABS: {
        VERIFY(!call.arguments.empty(), "Arity mismatch not verified during resolution");
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
    case syntax::token_type_t::BUILTIN_CLZ: {
        VERIFY(!call.arguments.empty(), "Arity mismatch not verified during resolution");
        const auto expr_h{call.arguments.front().as_opt<ast::expr_handle>()};
        if (!expr_h) { return stdx::none; }
        const auto arg{try_eval(*expr_h)};
        if (!arg) { return stdx::none; }
        if (!arg->is<u64>() && !arg->is<i64>()) { return stdx::none; }
        const auto v{arg->is<u64>() ? arg->as<u64>() : static_cast<u64>(arg->as<i64>())};
        return const_value{static_cast<u64>(std::countl_zero(v)), usize_type};
    }
    case syntax::token_type_t::BUILTIN_CTZ: {
        VERIFY(!call.arguments.empty(), "Arity mismatch not verified during resolution");
        const auto expr_h{call.arguments.front().as_opt<ast::expr_handle>()};
        if (!expr_h) { return stdx::none; }
        const auto arg{try_eval(*expr_h)};
        if (!arg) { return stdx::none; }
        if (!arg->is<u64>() && !arg->is<i64>()) { return stdx::none; }
        const auto v{arg->is<u64>() ? arg->as<u64>() : static_cast<u64>(arg->as<i64>())};
        return const_value{static_cast<u64>(std::countr_zero(v)), usize_type};
    }
    case syntax::token_type_t::BUILTIN_POP_COUNT: {
        VERIFY(!call.arguments.empty(), "Arity mismatch not verified during resolution");
        const auto expr_h{call.arguments.front().as_opt<ast::expr_handle>()};
        if (!expr_h) { return stdx::none; }
        const auto arg{try_eval(*expr_h)};
        if (!arg) { return stdx::none; }
        if (!arg->is<u64>() && !arg->is<i64>()) { return stdx::none; }
        const auto v{arg->is<u64>() ? arg->as<u64>() : static_cast<u64>(arg->as<i64>())};
        return const_value{static_cast<u64>(std::popcount(v)), usize_type};
    }
    case syntax::token_type_t::BUILTIN_MIN:
    case syntax::token_type_t::BUILTIN_MAX:
    case syntax::token_type_t::BUILTIN_DIV_TRUNC:
    case syntax::token_type_t::BUILTIN_DIV_FLOOR:
    case syntax::token_type_t::BUILTIN_REM:
    case syntax::token_type_t::BUILTIN_MOD:       {
        VERIFY(call.arguments.size() >= 2, "Arity mismatch not verified during resolution");
        const auto a_h{call.arguments[0].as_opt<ast::expr_handle>()};
        const auto b_h{call.arguments[1].as_opt<ast::expr_handle>()};
        if (!a_h || !b_h) { return stdx::none; }
        const auto a{try_eval(*a_h)};
        const auto b{try_eval(*b_h)};
        if (!a || !b) { return stdx::none; }

        const auto tok{builtin_type};
        const auto fold_int = [&](auto av, auto bv) -> stdx::option<const_value> {
            using T = decltype(av);
            switch (tok) {
            case syntax::token_type_t::BUILTIN_MIN:
                return const_value{av < bv ? av : bv, a->get_type()};
            case syntax::token_type_t::BUILTIN_MAX:
                return const_value{av > bv ? av : bv, a->get_type()};
            default: break;
            }
            if (bv == T{0}) {
                const bool is_rem{tok == syntax::token_type_t::BUILTIN_REM ||
                                  tok == syntax::token_type_t::BUILTIN_MOD};
                ctx_.diags.emplace_back(
                    fmt::format("{} by zero in compile-time constant expression",
                                is_rem ? "Modulo" : "Division"),
                    sema::error::CONSTEXPR_EVALUATION_FAILED,
                    module_->ast.location_of(id));
                return const_value::make_poison();
            }
            switch (tok) {
            case syntax::token_type_t::BUILTIN_DIV_TRUNC:
                return const_value{av / bv, a->get_type()};
            case syntax::token_type_t::BUILTIN_REM:       return const_value{av % bv, a->get_type()};
            case syntax::token_type_t::BUILTIN_DIV_FLOOR: {
                auto q{av / bv};
                if constexpr (Signed<T>) {
                    if ((av % bv != 0) && ((av < 0) != (bv < 0))) { --q; }
                }
                return const_value{q, a->get_type()};
            }
            case syntax::token_type_t::BUILTIN_MOD: {
                auto r{av % bv};
                if constexpr (Signed<T>) {
                    if ((r != 0) && ((r < 0) != (bv < 0))) { r += bv; }
                }
                return const_value{r, a->get_type()};
            }
            default: return stdx::none;
            }
        };

        if (a->is<i64>() && b->is<i64>()) { return fold_int(a->as<i64>(), b->as<i64>()); }
        if (a->is<u64>() && b->is<u64>()) { return fold_int(a->as<u64>(), b->as<u64>()); }
        if ((tok == syntax::token_type_t::BUILTIN_MIN ||
             tok == syntax::token_type_t::BUILTIN_MAX) &&
            a->is<f64>() && b->is<f64>()) {
            const auto av{a->as<f64>()};
            const auto bv{b->as<f64>()};
            const bool want_max{tok == syntax::token_type_t::BUILTIN_MAX};
            return const_value{want_max ? (av > bv ? av : bv) : (av < bv ? av : bv), a->get_type()};
        }
        return stdx::none;
    }
    case syntax::token_type_t::BUILTIN_MUL_ADD: {
        VERIFY(call.arguments.size() >= 4, "Arity mismatch not verified during resolution");
        const auto a_h{call.arguments[1].as_opt<ast::expr_handle>()};
        const auto b_h{call.arguments[2].as_opt<ast::expr_handle>()};
        const auto c_h{call.arguments[3].as_opt<ast::expr_handle>()};
        if (a_h && b_h && c_h) {
            const auto a{try_eval(*a_h)};
            const auto b{try_eval(*b_h)};
            const auto c{try_eval(*c_h)};
            if (a && b && c) {
                if (a->is<f64>() && b->is<f64>() && c->is<f64>()) {
                    return const_value{std::fma(a->as<f64>(), b->as<f64>(), c->as<f64>()),
                                       a->get_type()};
                }
                if (a->is<i64>() && b->is<i64>() && c->is<i64>()) {
                    return const_value{(a->as<i64>() * b->as<i64>()) + c->as<i64>(), a->get_type()};
                }
                if (a->is<u64>() && b->is<u64>() && c->is<u64>()) {
                    return const_value{(a->as<u64>() * b->as<u64>()) + c->as<u64>(), a->get_type()};
                }
            }
        }
        return stdx::none;
    }
    case syntax::token_type_t::BUILTIN_TAG_NAME: {
        VERIFY(!call.arguments.empty(), "Arity mismatch not verified during resolution");
        if (const auto expr_h{call.arguments.front().as_opt<ast::expr_handle>()}) {
            if (const auto val{try_eval(*expr_h)}) {
                if (const auto e_val{val->as_opt<const_enum>()}) {
                    return const_value::make_string(ctx_, e_val->name);
                }
                if (const auto u_val{val->as_opt<const_union>()}) {
                    return const_value::make_string(ctx_, u_val->active_field);
                }
            }
        }
        return stdx::none;
    }
    case syntax::token_type_t::BUILTIN_TYPE_NAME: {
        VERIFY(!call.arguments.empty(), "Arity mismatch not verified during resolution");
        const auto&               arg{call.arguments.front()};
        stdx::option<sema::type&> target_type;
        if (const auto type_id{arg.as_opt<ast::explicit_type_id>()}) {
            target_type = module_->get_sema_type_opt(*type_id);
        } else if (const auto expr_h{arg.as_opt<ast::expr_handle>()}) {
            target_type = module_->get_sema_type_opt(*expr_h);
        }
        if (!target_type) { return stdx::none; }

        // Match the resolver: a fixed-length, null-terminated byte array (like a string literal).
        auto  name{ctx_.type_display_name(*target_type)};
        auto& t_u8{ctx_.get_int(8, false)};
        auto& arr_type{ctx_.get_array(sema::types::mut::CONSTANT, true, name.size() + 1, t_u8)};
        return const_value{std::move(name), arr_type};
    }
    case syntax::token_type_t::BUILTIN_TARGET_OS: {
        const auto facts{codegen::target_facts::resolve(ctx_.target_opts.triple_str)};
        return target_enum_value("Os", facts.os);
    }
    case syntax::token_type_t::BUILTIN_TARGET_ARCH: {
        const auto facts{codegen::target_facts::resolve(ctx_.target_opts.triple_str)};
        return target_enum_value("Arch", facts.arch);
    }
    case syntax::token_type_t::BUILTIN_TARGET_TRIPLE: {
        const auto triple{codegen::resolve_target_triple(ctx_.target_opts.triple_str)};
        return const_value::make_string(ctx_, triple.str());
    }
    case syntax::token_type_t::BUILTIN_TARGET_ABI: {
        const auto facts{codegen::target_facts::resolve(ctx_.target_opts.triple_str)};
        return target_enum_value("Abi", facts.abi);
    }
    case syntax::token_type_t::BUILTIN_TARGET_FAMILY: {
        const auto facts{codegen::target_facts::resolve(ctx_.target_opts.triple_str)};
        return target_enum_value("Family", facts.family);
    }
    case syntax::token_type_t::BUILTIN_TARGET_ENDIAN: {
        const auto facts{codegen::target_facts::resolve(ctx_.target_opts.triple_str)};
        return target_enum_value("Endian", facts.endian);
    }
    case syntax::token_type_t::BUILTIN_TARGET_PTR_BITS: {
        const auto facts{codegen::target_facts::resolve(ctx_.target_opts.triple_str)};
        return const_value{u64{facts.ptr_bits},
                           ctx_.get_builtin_resolved_type(sema::type_kind::USIZE)};
    }
    case syntax::token_type_t::BUILTIN_SRC: {
        const auto   loc{id.is_valid() ? module_->ast.location_of(id)
                                       : module_->ast.location_of(call.function)};
        const_struct src_struct;
        auto&        t_u32{ctx_.get_int(32, false)};
        src_struct.fields["file"]   = const_value::make_string(ctx_, module_->path.string());
        src_struct.fields["line"]   = const_value{static_cast<u64>(loc.line), t_u32};
        src_struct.fields["column"] = const_value{static_cast<u64>(loc.column), t_u32};
        return const_value{std::move(src_struct), ctx_.get_builtin_type("SourceLocation")};
    }
    case syntax::token_type_t::BUILTIN_SET_EVAL_RECURSION_LIMIT: {
        VERIFY(!call.arguments.empty(), "Arity mismatch not verified during resolution");
        const auto expr_h{call.arguments.front().as_opt<ast::expr_handle>()};
        if (expr_h) {
            if (const auto val{try_eval(*expr_h)}) {
                if (const auto limit{val->as_int_opt()}) {
                    if (*limit > 0) {
                        recursion_limit_stack_.emplace_back(max_recursion_depth_);
                        max_recursion_depth_ = static_cast<usize>(*limit);
                    }
                }
            }
        }
        return const_value{void_val{}, ctx_.get_builtin_resolved_type(sema::type_kind::VOID_)};
    }
    case syntax::token_type_t::BUILTIN_PTR_FROM_INT: {
        // `@ptrFromInt(T, n)` -> the pointer whose address bits are the constant `n`.
        if (call.arguments.size() < 2) { return stdx::none; }
        const auto op_h{call.arguments[1].as_opt<ast::expr_handle>()};
        if (!op_h) { return stdx::none; }
        const auto operand{try_eval(*op_h)};
        if (!operand) { return stdx::none; }
        const auto bits{operand->as_int_opt()};
        if (!bits) { return stdx::none; }
        auto target{id.is_valid() ? module_->get_sema_type_opt(id) : stdx::none};
        if (!target) { target = module_->get_sema_type_opt(call.function); }
        return const_value{static_cast<u64>(static_cast<u128>(*bits)), target};
    }
    case syntax::token_type_t::BUILTIN_INT_FROM_PTR: {
        // `@intFromPtr(p)` folds only when `p` is itself a folded pointer constant.
        if (call.arguments.empty()) { return stdx::none; }
        const auto op_h{call.arguments[0].as_opt<ast::expr_handle>()};
        if (!op_h) { return stdx::none; }
        const auto operand{try_eval(*op_h)};
        if (!operand) { return stdx::none; }
        if (operand->is<nullptr_val>()) { return const_value{u64{0}, usize_type}; }
        const auto bits{operand->as_uint_opt()};
        if (!bits) { return stdx::none; }
        return const_value{static_cast<u64>(*bits), usize_type};
    }
    case syntax::token_type_t::BUILTIN_AS:
    case syntax::token_type_t::BUILTIN_BIT_CAST: {
        // Fold the numeric/pointer subset; leave floats, enums and aggregates to the emitter.
        if (call.arguments.size() < 2) { return stdx::none; }
        const auto op_h{call.arguments[1].as_opt<ast::expr_handle>()};
        if (!op_h) { return stdx::none; }
        const auto operand{try_eval(*op_h)};
        if (!operand) { return stdx::none; }
        const auto src_int{operand->as_int_opt()};
        if (!src_int) { return stdx::none; }
        auto target{id.is_valid() ? module_->get_sema_type_opt(id) : stdx::none};
        if (!target) { target = module_->get_sema_type_opt(call.function); }
        if (!target) { return stdx::none; }
        const auto ptr_bits{static_cast<u32>(
            codegen::resolve_target_triple(ctx_.target_opts.triple_str).isArch64Bit() ? 64 : 32)};
        if (const auto w{integer_target_width(*target, ptr_bits)}) {
            return wrap_to_width(*operand, w->first, w->second, target);
        }
        if (target->get_kind() == sema::type_kind::BOOL) {
            return const_value{*src_int != 0,
                               ctx_.get_builtin_resolved_type(sema::type_kind::BOOL)};
        }
        if (target->get_kind() == sema::type_kind::POINTER) {
            return const_value{static_cast<u64>(static_cast<u128>(*src_int)), target};
        }
        return stdx::none;
    }
    case syntax::token_type_t::BUILTIN_SET_MAIN_SYMBOL: {
        VERIFY(!call.arguments.empty(), "Arity mismatch not verified during resolution");
        const auto expr_h{call.arguments.front().as_opt<ast::expr_handle>()};
        if (expr_h) {
            if (const auto val{try_eval(*expr_h)}) {
                if (const auto str{val->as_opt<std::string>()}) {
                    if (!syntax::token_type::is_valid_identifier_name(*str)) {
                        ctx_.diags.emplace_back(
                            fmt::format(
                                "@setMainSymbol argument must be a valid identifier; found '{}'",
                                *str),
                            sema::error::TYPE_MISMATCH,
                            module_->ast.location_of(*expr_h));
                        return const_value::make_poison();
                    }
                    ctx_.user_main_name = *str;
                }
            }
        }
        return const_value{void_val{}, ctx_.get_builtin_resolved_type(sema::type_kind::VOID_)};
    }
    default: return stdx::none;
    }
}

auto const_eval::lookup_bound_callable(std::string_view name) -> stdx::option<bound_callable> {
    PROFILE_FUNCTION();
    // Find a constexpr callable value bound to `name` (call frame first, then constexpr frame).
    stdx::option<const const_value&> v;
    for (auto& fr : std::views::reverse(call_stack_)) {
        if (auto it{fr.bindings.find(name)}; it != fr.bindings.end()) {
            v.emplace(it->second);
            break;
        }
    }
    if (!v) { v = ctx_.lookup_constexpr_binding(name); }
    if (!v) { return stdx::none; }

    if (const auto cl{v->as_opt<const_closure>()}) {
        auto& m{cl->module ? *cl->module : *module_};
        if (const auto fe{m.ast.get_as_opt<ast::function_expr>(cl->fn_node)}) {
            return bound_callable{.module = &m, .fn_expr = fe.get(), .captures = cl->captures};
        }
        return stdx::none;
    }
    const auto sref{v->as_opt<std::string>()};
    if (!sref) { return stdx::none; }

    // A top-level `const` function by name (globals decay to a name string).
    if (!module_->root_table_idx) { return stdx::none; }
    const auto& table{ctx_.registry.get(*module_->root_table_idx)};
    const auto  sym{table.get_opt(*sref)};
    if (!sym) { return stdx::none; }
    const auto sn{sym->get_data().as_opt<sema::symbols::node_t>()};
    if (!sn) { return stdx::none; }
    const auto decl{module_->ast.get_as_opt<ast::decl_stmt>(*sn)};
    if (!decl || !decl->value) { return stdx::none; }
    if (const auto fe{module_->ast.get_as_opt<ast::function_expr>(*decl->value)}) {
        return bound_callable{.module = module_, .fn_expr = fe.get()};
    }
    return stdx::none;
}

auto const_eval::eval_constexpr_fn(ast::node_id                      call_id,
                                   const ast::function_expr&         fn_expr,
                                   const std::vector<const_value>&   args,
                                   stdx::option<const const_struct&> captures)
    -> stdx::option<const_value> {
    PROFILE_FUNCTION();
    VERIFY(fn_expr.parameters.size() == args.size(),
           "Constexpr function args count must match parameters count");

    if (recursion_depth_ >= max_recursion_depth_) {
        const auto loc{call_id.is_valid() ? module_->ast.location_of(call_id)
                                          : module_->ast.location_of(fn_expr.body)};
        ctx_.diags.emplace_back("Constexpr function recursion limit exceeded",
                                sema::error::CONSTEXPR_RECURSION_LIMIT_EXCEEDED,
                                loc);
        return const_value::make_poison();
    }

    call_frame frame;
    for (const auto& [param, arg] : std::views::zip(fn_expr.parameters, args)) {
        const auto& ident{module_->ast.get_as<ast::identifier_expr>(param.name)};
        frame.bindings.emplace(ident.name, arg);
    }
    if (captures) {
        for (const auto& [name, val] : captures->fields) { frame.bindings.emplace(name, val); }
    }

    const usize prev_stack_size{recursion_limit_stack_.size()};
    call_stack_.emplace_back(std::move(frame));
    const default_counter::guard g{recursion_depth_};
    const auto                   block_res{eval_stmt(fn_expr.body)};
    call_stack_.pop_back();
    while (recursion_limit_stack_.size() > prev_stack_size) {
        max_recursion_depth_ = recursion_limit_stack_.back();
        recursion_limit_stack_.pop_back();
    }
    return block_res;
}

auto const_eval::eval_stmt(const ast::stmt_handle& stmt) -> stdx::option<const_value> {
    PROFILE_FUNCTION();
    return module_->ast[*stmt].visit(
        [&](const auto&) -> stdx::option<const_value> { return stdx::none; },
        [&](const ast::block_stmt& data) { return eval_block(*stmt, data); },
        [&](const ast::decl_stmt& data) { return eval_decl(*stmt, data); },
        [&](const ast::return_stmt& data) -> stdx::option<const_value> {
            if (data.expression) { return try_eval(*data.expression); }
            return const_value{void_val{}, ctx_.get_builtin_resolved_type(sema::type_kind::VOID_)};
        },
        [&](const ast::discard_stmt& data) -> stdx::option<const_value> {
            if (module_->ast[data.discarded].template is<ast::if_expr>() ||
                module_->ast[data.discarded].template is<ast::while_loop_expr>() ||
                module_->ast[data.discarded].template is<ast::do_while_loop_expr>() ||
                module_->ast[data.discarded].template is<ast::for_loop_expr>() ||
                module_->ast[data.discarded].template is<ast::match_expr>()) {
                return try_eval(data.discarded);
            }
            DISCARD(try_eval(data.discarded));
            return stdx::none;
        },
        [&](const ast::expr_stmt& data) -> stdx::option<const_value> {
            const auto expr_id{data.expression};
            if (module_->ast[expr_id].template is<ast::if_expr>() ||
                module_->ast[expr_id].template is<ast::while_loop_expr>() ||
                module_->ast[expr_id].template is<ast::do_while_loop_expr>() ||
                module_->ast[expr_id].template is<ast::for_loop_expr>() ||
                module_->ast[expr_id].template is<ast::match_expr>()) {
                return try_eval(expr_id);
            }
            DISCARD(try_eval(expr_id));
            return stdx::none;
        });
}

auto const_eval::eval_block(ast::node_id, const ast::block_stmt& block)
    -> stdx::option<const_value> {
    PROFILE_FUNCTION();
    for (const auto& stmt : block.statements) {
        if (const auto res{eval_stmt(stmt)}) { return res; }
    }
    return stdx::none;
}

auto const_eval::eval_decl(ast::node_id, const ast::decl_stmt& decl) -> stdx::option<const_value> {
    PROFILE_FUNCTION();
    if (decl.value && !call_stack_.empty()) {
        if (const auto val{try_eval(*decl.value)}) {
            const auto& ident{module_->ast.get_as<ast::identifier_expr>(decl.name)};
            call_stack_.back().bindings.insert_or_assign(ident.name, *val);
        }
    }
    return stdx::none;
}

auto const_eval::eval_if(ast::node_id, const ast::if_expr& if_expr) -> stdx::option<const_value> {
    PROFILE_FUNCTION();
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
    PROFILE_FUNCTION();
    while (true) {
        const auto cond{try_eval(loop.condition)};
        if (!cond || !cond->is<bool>()) { return stdx::none; }
        if (!cond->as<bool>()) { break; }

        if (const auto body_res{eval_stmt(loop.block)}) { return body_res; }

        if (loop.continuation) { DISCARD(try_eval(*loop.continuation)); }
    }
    return stdx::none;
}

auto const_eval::eval_do_while(ast::node_id, const ast::do_while_loop_expr& loop)
    -> stdx::option<const_value> {
    PROFILE_FUNCTION();
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
    PROFILE_FUNCTION();
    VERIFY(loop.iterables.size() == loop.captures.size(),
           "For loop iterables and captures must match in count");
    if (loop.iterables.empty()) { return stdx::none; }

    std::vector<std::vector<const_value>> iterable_sequences;
    iterable_sequences.reserve(loop.iterables.size());

    for (const auto& iterable_handle : loop.iterables) {
        std::vector<const_value> sequence;
        const auto               iterable_id{*iterable_handle};

        if (const auto range{module_->ast.get_as_opt<ast::range_expr>(iterable_id)}) {
            if (!range->lhs || !range->rhs) { return stdx::none; }
            const auto start_val{try_eval(*range->lhs)};
            const auto end_val{try_eval(*range->rhs)};
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
        } else if (const auto array{module_->ast.get_as_opt<ast::array_expr>(iterable_id)}) {
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
                const auto& ident{module_->ast.get_as<ast::identifier_expr>(capture.payload)};
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
