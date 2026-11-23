#include <array>
#include <bit>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/target.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/layout.hh"
#include "compiler/gir/semantics.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/big_int.hh"
#include "support/float128.hh"
#include "support/float_math.hh"
#include "support/int128.hh"

namespace ghoti::uir::sema {

namespace semantics = gir::semantics;

namespace {

// A typed integer's bits in its own domain
[[nodiscard]] auto domain_bits(const big_int& v, semantics::int_domain domain) -> u128 {
    return semantics::to_domain(v.is_negative() ? static_cast<u128>(*v.to_i128()) : *v.to_u128(),
                                domain);
}

[[nodiscard]] auto from_domain(u128 bits, semantics::int_domain domain) -> big_int {
    return domain.is_signed ? big_int{semantics::signed_value(bits, domain)}
                            : big_int{semantics::unsigned_value(bits, domain)};
}

} // namespace

auto analyzer::builtin_bit_count(syntax::builtin_t    which,
                                 gsl::span<const ref> args,
                                 const source&        src) -> result<operand> {
    const auto name{syntax::builtin_name(which)};
    if (args.size() != 1) { return comp_.unsupported(u_, src, "a math builtin's arity"); }
    const auto given{TRY(resolve_value(args.front(), src))};
    const auto arg{TRY(read_through_reference(given, src))};
    auto&      type{type_of(arg)};
    auto&      ctx{comp_.ctx()};
    const auto kind{type.get_kind()};
    if (gs::is_comptime_numeric(kind) || kind == gs::type_kind::VECTOR) {
        return comp_.unsupported(u_, src, fmt::format("`{}` of an untyped value or vector", name));
    }

    if (!gs::is_integer(kind)) {
        return comp_.report(u_,
                            src,
                            gs::error::TYPE_MISMATCH,
                            fmt::format("'{}' operand must be an integer type; found '{}'",
                                        name,
                                        ctx.type_display_name(type)));
    }

    const auto domain{domain_of(type)};
    auto&      result_type{ctx.get_int(static_cast<u16>(std::bit_width(u64{domain.bits})), false)};
    const auto op{which == syntax::builtin_t::CLZ   ? semantics::int_unary_op::CLZ
                  : which == syntax::builtin_t::CTZ ? semantics::int_unary_op::CTZ
                                                    : semantics::int_unary_op::POPCOUNT};
    if (const auto known{comptime_value(arg)}) {
        const auto value{int_of(comp_.constants().value_at(*known))};
        if (!value || !folds_exactly(type)) {
            return comp_.unsupported(u_, src, fmt::format("`{}` of this value", name));
        }

        const auto count{semantics::fold_int_unary(op, domain, domain_bits(*value, domain))};
        if (!count) { return comp_.unsupported(u_, src, fmt::format("`{}` of this value", name)); }
        return operand{intern_int(comp_.constants(), big_int{*count}, result_type)};
    }

    TRY(require_runtime(src));
    std::vector<gir::value> call_args;
    call_args.emplace_back(TRY(as_runtime(arg, src)));
    const auto result{builder_.emit_builtin_call(name, std::move(call_args), result_type)};
    return operand{gir::value{*result, result_type}};
}

auto analyzer::builtin_abs(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (args.size() != 1) { return comp_.unsupported(u_, src, "a math builtin's arity"); }
    const auto given{TRY(resolve_value(args.front(), src))};
    const auto arg{TRY(read_through_reference(given, src))};
    auto&      type{type_of(arg)};
    auto&      pool{comp_.constants()};
    if (!gs::is_numeric(type.get_kind())) {
        return comp_.unsupported(u_, src, "`@abs` of something other than a number");
    }

    if (const auto known{comptime_value(arg)}) {
        const auto& value{pool.value_at(*known)};
        if (const auto f{value.data.as_opt<f128>()}) {
            const f128 magnitude{f->abs()};
            return operand{pool.intern(magnitude, type)};
        }

        const auto i{int_of(value)};
        if (!i) { return comp_.unsupported(u_, src, "`@abs` of this value"); }
        if (gs::is_comptime_numeric(type.get_kind())) {
            return operand{intern_int(pool, i->abs(), type)};
        }

        if (!folds_exactly(type)) { return comp_.unsupported(u_, src, "`@abs` past 128 bits"); }
        const auto domain{domain_of(type)};
        const auto folded{semantics::fold_int_unary(
            semantics::int_unary_op::ABS, domain, domain_bits(*i, domain))};
        if (folded) { return operand{intern_int(pool, from_domain(*folded, domain), type)}; }
        if (is_comptime()) {
            return comp_.report(
                u_,
                src,
                gs::error::COMPTIME_EVALUATION_FAILED,
                semantics::describe(folded.error(), domain, comp_.ctx().type_display_name(type)));
        }
    }

    TRY(require_runtime(src));
    if (gs::is_comptime_numeric(type.get_kind())) {
        return comp_.unsupported(u_, src, "`@abs` of an untyped value at runtime");
    }

    // `@abs(MIN)` panics like the negation it mirrors
    const bool checked{
        comp_.ctx().runtime_safety && gs::is_integer(type.get_kind()) &&
        semantics::any(semantics::runtime_checks(
            semantics::int_unary_op::ABS, {.bits = 1, .is_signed = gs::is_signed_integer(type)}))};
    std::vector<gir::value> call_args;
    call_args.emplace_back(TRY(as_runtime(arg, src)));
    const auto result{builder_.emit_builtin_call(
        "@abs", std::move(call_args), type, stdx::none, stdx::none, stdx::none, checked)};
    return operand{gir::value{*result, type}};
}

auto analyzer::builtin_binary_math(syntax::builtin_t    which,
                                   gsl::span<const ref> args,
                                   const source&        src) -> result<operand> {
    const auto name{syntax::builtin_name(which)};
    if (args.size() != 2) { return comp_.unsupported(u_, src, "a math builtin's arity"); }
    const auto lhs_given{TRY(resolve_value(args[0], src))};
    const auto rhs_given{TRY(resolve_value(args[1], src))};
    const auto lhs{TRY(read_through_reference(lhs_given, src))};
    const auto rhs{TRY(read_through_reference(rhs_given, src))};
    auto&      ctx{comp_.ctx()};
    auto&      pool{comp_.constants()};

    const bool floats_ok{which == syntax::builtin_t::MIN || which == syntax::builtin_t::MAX};
    auto&      lhs_type{type_of(lhs)};
    auto&      rhs_type{type_of(rhs)};
    const auto accepts{[&](const gs::type& t) {
        return floats_ok
                   ? gs::is_numeric(t.get_kind())
                   : gs::is_integer(t.get_kind()) || t.get_kind() == gs::type_kind::COMPTIME_INT;
    }};
    if (lhs_type.get_kind() == gs::type_kind::VECTOR ||
        rhs_type.get_kind() == gs::type_kind::VECTOR) {
        return comp_.unsupported(u_, src, fmt::format("`{}` of a vector", name));
    }

    if (!accepts(lhs_type) || !accepts(rhs_type)) {
        return comp_.report(u_,
                            src,
                            gs::error::OPERATOR_TYPE_MISMATCH,
                            fmt::format("'{}' expects two {} operands; found '{}' and '{}'",
                                        name,
                                        floats_ok ? "numeric" : "integer",
                                        ctx.type_display_name(lhs_type),
                                        ctx.type_display_name(rhs_type)));
    }

    const std::array operands{lhs, rhs};
    auto&            peer{*TRY(peer_type(operands, src))};
    const bool       both_known{comptime_value(lhs) && comptime_value(rhs)};
    auto&            work{both_known || !gs::is_comptime_numeric(peer.get_kind())
                              ? peer
                              : ctx.default_concrete(peer)};
    const auto       l{TRY(coerce_to(lhs, work, src, {.what = use_site::kind::STORE}))};
    const auto       r{TRY(coerce_to(rhs, work, src, {.what = use_site::kind::STORE}))};

    if (both_known) {
        const auto& lv{pool.value_at(*comptime_value(l))};
        const auto& rv{pool.value_at(*comptime_value(r))};
        if (const auto lf{lv.data.as_opt<f128>()}) {
            const auto rf{rv.data.as_opt<f128>()};
            const auto op{semantics::float_op_of(which)};
            const auto format{gs::float_format_of(work)};
            if (!rf || !op) {
                return comp_.unsupported(u_, src, fmt::format("`{}` of these values", name));
            }
            const f128 left{*lf};
            const f128 right{*rf};
            return operand{pool.intern(
                semantics::fold_float(*op, format.value_or(float_format::QUAD), left, right),
                work)};
        }

        const auto li{int_of(lv)};
        const auto ri{int_of(rv)};
        if (!li || !ri) {
            return comp_.unsupported(u_, src, fmt::format("`{}` of these values", name));
        }
        if (work.get_kind() == gs::type_kind::COMPTIME_INT) {
            if (!floats_ok) {
                return comp_.unsupported(u_, src, fmt::format("`{}` of untyped integers", name));
            }

            const bool less{*li < *ri};
            const bool pick_left{which == syntax::builtin_t::MIN ? less : !less};
            return operand{intern_int(pool, pick_left ? *li : *ri, work)};
        }

        const auto op{semantics::int_op_of(which)};
        if (!op || !folds_exactly(work)) {
            return comp_.unsupported(u_, src, fmt::format("`{}` of these values", name));
        }

        const auto domain{domain_of(work)};
        const auto folded{
            semantics::fold_int(*op, domain, domain_bits(*li, domain), domain_bits(*ri, domain))};
        if (folded) { return operand{intern_int(pool, from_domain(*folded, domain), work)}; }
        if (is_comptime()) {
            return comp_.report(
                u_,
                src,
                gs::error::COMPTIME_EVALUATION_FAILED,
                semantics::describe(folded.error(), domain, ctx.type_display_name(work)));
        }
    }

    TRY(require_runtime(src));

    // Division by zero and `MIN / -1` panic like the operators they mirror
    const auto int_op{semantics::int_op_of(which)};
    const bool checked{ctx.runtime_safety && gs::is_integer(work.get_kind()) && int_op &&
                       semantics::any(semantics::runtime_checks(
                           *int_op, {.bits = 1, .is_signed = gs::is_signed_integer(work)}))};
    std::vector<gir::value> call_args;
    call_args.emplace_back(TRY(as_runtime(l, src)));
    call_args.emplace_back(TRY(as_runtime(r, src)));
    const auto result{builder_.emit_builtin_call(
        name, std::move(call_args), work, stdx::none, stdx::none, stdx::none, checked)};
    return operand{gir::value{*result, work}};
}

auto analyzer::builtin_float_math(syntax::builtin_t    which,
                                  gsl::span<const ref> args,
                                  const source&        src) -> result<operand> {
    const auto name{syntax::builtin_name(which)};
    if (args.size() != 1) { return comp_.unsupported(u_, src, "a math builtin's arity"); }
    const auto given{TRY(resolve_value(args.front(), src))};
    const auto arg{TRY(read_through_reference(given, src))};
    auto&      type{type_of(arg)};
    if (!gs::is_float(type.get_kind())) {
        return comp_.unsupported(
            u_, src, fmt::format("`{}` of something other than a typed float", name));
    }

    const auto function{*semantics::math_function_of(which)};
    if (const auto known{comptime_value(arg)}) {
        // Only the functions that survive a second rounding fold exactly
        const bool exact_enough{function == math_function::SQRT ||
                                function == math_function::FLOOR ||
                                function == math_function::CEIL};
        const auto input{comp_.constants().value_at(*known).data.as_opt<f128>()};
        const auto format{gs::float_format_of(type)};
        if (!exact_enough || !input || !format) {
            return comp_.unsupported(u_, src, fmt::format("`{}` folded at compile time", name));
        }

        const f128 x{*input};
        const auto folded{evaluate_traced(function, x, *format)};
        if (folded.overflowed) {
            return comp_.unsupported(u_, src, fmt::format("`{}` past its range", name));
        }
        return operand{comp_.constants().intern(folded.value, type)};
    }

    TRY(require_runtime(src));
    std::vector<gir::value> call_args;
    call_args.emplace_back(TRY(as_runtime(arg, src)));
    const auto result{builder_.emit_builtin_call(name, std::move(call_args), type)};
    return operand{gir::value{*result, type}};
}

auto analyzer::byte_count(const gir::value& count, usize elem_bytes) -> gir::value {
    if (elem_bytes == 1) { return count; }
    auto& usize_type{builtin(gs::type_kind::USIZE)};
    return gir::value{builder_.emit_binary(gir::instruction_kind::MUL,
                                           count,
                                           usize_constant(static_cast<u64>(elem_bytes)),
                                           usize_type),
                      usize_type};
}

auto analyzer::analyze_mem_view(inst_id inst) -> result<operand> {
    const auto& src{src_of(inst)};
    const auto  arg_ref{mod().data_of(inst).un.operand};
    const bool  is_dest{mod().data_of(inst).un.aux != 0};
    const auto  given{TRY(resolve_value(arg_ref, src))};
    if (is_comptime()) { return comp_.unsupported(u_, src, "a memory builtin at compile time"); }
    mem_views_.insert_or_assign(inst, TRY(buffer_view(arg_ref, given, is_dest, src)));
    return void_value();
}

auto analyzer::buffer_view(ref arg_ref, const operand& given, bool is_dest, const source& src)
    -> result<mem_view> {
    auto&      ctx{comp_.ctx()};
    auto&      usize_type{builtin(gs::type_kind::USIZE)};
    auto&      byte_ptr{ctx.get_pointer(gs::types::mut::MUTABLE, ctx.get_int(8, false))};
    const auto layout{codegen::type_layout::resolve(ctx.target_opts.triple_str)};
    auto&      type{type_of(given)};
    if (const auto arr{type.get_data().as_opt<gs::types::array>()}) {
        // An array is reached where it lives
        const auto base{TRY(address_of(arg_ref, src))};
        const auto first{
            builder_.emit_get_element_ptr(base.address, {usize_constant(0)}, byte_ptr)};
        const auto count{usize_constant(arr->len)};
        return mem_view{
            .data  = gir::value{first, byte_ptr},
            .count = count,
            .bytes = is_dest ? stdx::option<gir::value>{byte_count(
                                   count, gir::size_of(arr->underlying, layout))}
                             : stdx::none,
        };
    }

    const auto slice{type.get_data().as_opt<gs::types::slice>()};
    if (!slice) { return comp_.unsupported(u_, src, "a memory builtin's buffer of this type"); }
    const auto value{TRY(as_runtime(given, src))};
    const auto slot{spill(value, type, false).address};
    const auto ptr_at{
        builder_.emit_get_element_ptr(slot, {usize_constant(SLICE_PTR_FIELD)}, byte_ptr)};
    const auto len_at{
        builder_.emit_get_element_ptr(slot, {usize_constant(SLICE_LEN_FIELD)}, usize_type)};
    const gir::value data{builder_.emit_load(gir::value{ptr_at, byte_ptr}, byte_ptr), byte_ptr};
    const gir::value count{builder_.emit_load(gir::value{len_at, usize_type}, usize_type),
                           usize_type};
    return mem_view{.data  = data,
                    .count = count,
                    .bytes = is_dest ? stdx::option<gir::value>{byte_count(
                                           count, gir::size_of(slice->underlying, layout))}
                                     : stdx::none};
}

auto analyzer::builtin_mem(syntax::builtin_t which, gsl::span<const ref> args, const source& src)
    -> result<operand> {
    const auto name{syntax::builtin_name(which)};
    if (args.size() != 2) { return comp_.unsupported(u_, src, "a memory builtin's arity"); }
    TRY(require_runtime(src));
    const auto view_of{[&](ref r) -> stdx::option<mem_view> {
        const auto inst{as_inst(r)};
        const auto found{inst ? mem_views_.find(*inst) : mem_views_.end()};
        if (found == mem_views_.end()) { return stdx::none; }
        return found->second;
    }};

    auto&      ctx{comp_.ctx()};
    auto&      bool_type{builtin(gs::type_kind::BOOL)};
    auto&      void_type{builtin(gs::type_kind::VOID_)};
    const auto dest{view_of(args[0])};
    if (!dest) { return comp_.unsupported(u_, src, "a memory builtin's destination"); }

    if (!dest->bytes) { return comp_.unsupported(u_, src, "a memory builtin's destination"); }
    const auto bytes{*dest->bytes};

    if (which == syntax::builtin_t::MEMSET) {
        const auto fill_value{TRY(resolve_value(args[1], src))};
        const auto byte{TRY(
            coerce_to(fill_value, ctx.get_int(8, false), src, {.what = use_site::kind::STORE}))};
        const auto fill{TRY(as_runtime(byte, src))};
        DISCARD(builder_.emit_builtin_call(name, {dest->data, fill, bytes}, void_type));
        return void_value();
    }

    const auto from{view_of(args[1])};
    if (!from) { return comp_.unsupported(u_, src, "a memory builtin's source"); }

    // Both buffers must be as long, which runtime safety checks
    if (ctx.runtime_safety) {
        const gir::value equal{
            builder_.emit_binary(gir::instruction_kind::EQ, dest->count, from->count, bool_type),
            bool_type};
        TRY(guard(equal,
                  which == syntax::builtin_t::MEMCPY ? "@memcpy with mismatched slice lengths"
                                                     : "@memmove with mismatched slice lengths",
                  src));
    }

    DISCARD(builder_.emit_builtin_call(name, {dest->data, from->data, bytes}, void_type));
    return void_value();
}

} // namespace ghoti::uir::sema
