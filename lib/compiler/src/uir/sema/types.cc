#include <array>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/span>
#include <llvm/TargetParser/Triple.h>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/codegen/c_abi.hh"
#include "compiler/codegen/target.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/layout.hh"
#include "compiler/ip/index.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/big_int.hh"
#include "support/float128.hh"
#include "support/int128.hh"

namespace ghoti::uir::sema {

namespace {

// Two's-complement `u32` limbs, least significant first, packed into one `u128`
[[nodiscard]] auto limbs_value(gsl::span<const u32> limbs) noexcept -> u128 {
    u128 value{0};
    for (usize i{0}; i < limbs.size() && i < 4; ++i) {
        value |= static_cast<u128>(limbs[i]) << (32U * i);
    }

    return value;
}

// Every limb, for a literal wider than 128 bits
[[nodiscard]] auto wide_limbs_value(gsl::span<const u32> limbs) -> big_int {
    std::vector<u64> words((limbs.size() + 1) / 2);
    for (usize i{0}; i < limbs.size(); ++i) {
        words[i / 2] |= static_cast<u64>(limbs[i]) << (32U * (i % 2));
    }

    return big_int::from_words(words, words.size() * 64, false);
}

} // namespace

auto intern_int(ip::pool& pool, const big_int& value, gs::type& type) -> ip::index {
    if (const auto small{value.to_i128()}) {
        if (*small >= static_cast<i128>(std::numeric_limits<i64>::min()) &&
            *small <= static_cast<i128>(std::numeric_limits<i64>::max())) {
            return pool.intern(static_cast<i64>(*small), type);
        }

        if (*small >= 0 && *small <= static_cast<i128>(std::numeric_limits<u64>::max())) {
            return pool.intern(static_cast<u64>(*small), type);
        }

        return pool.intern(*small, type);
    }

    if (const auto wide{value.to_u128()}) { return pool.intern(*wide, type); }
    return pool.intern(value, type);
}

auto float_of_int(const big_int& value, float_format format) -> f128 {
    if (const auto small{value.to_i128()}) {
        return gir::semantics::float_from_int(
            {.bits = 128, .is_signed = true}, static_cast<u128>(*small), format);
    }

    if (const auto wide{value.to_u128()}) {
        return gir::semantics::float_from_int({.bits = 128, .is_signed = false}, *wide, format);
    }

    // Past 128 bits the digits round once, as a literal's do
    return f128::parse(value.to_string(), format).value;
}

auto wrap_int(const big_int& value, u64 bits, bool is_signed) -> big_int {
    return big_int::from_words(value.to_words(bits), bits, is_signed);
}

auto analyzer::analyze_int_value(inst_id inst) -> result<operand> {
    const big_int value{static_cast<u128>(mod().data_of(inst).int_value)};
    return intern_int(comp_.constants(), value, builtin(gs::type_kind::COMPTIME_INT));
}

auto analyzer::negated_to_fit(inst_id inst, const big_int& value, const gs::type& type) const
    -> bool {
    const inst_id next{std::to_underlying(inst) + 1};
    if (std::to_underlying(next) >= mod().instruction_count() || mod().tag_of(next) != tag::neg) {
        return false;
    }

    return mod().data_of(next).un.operand == inst_ref(inst) &&
           gs::comptime_int_fits(-value, type, comp_.ptr_bits());
}

auto analyzer::analyze_int_lit(inst_id inst) -> result<operand> {
    auto          rec{record(inst)};
    const auto    limbs{rec.words_field()};
    const auto    width{rec.u32_field()};
    const auto    bits{rec.u32_field()};
    const auto&   src{src_of(inst)};
    const big_int value{(bits & flags::INT_BIG) != 0 ? wide_limbs_value(limbs)
                                                     : big_int{limbs_value(limbs)}};
    const bool    is_signed{(bits & flags::INT_SIGNED) != 0};
    auto&         ctx{comp_.ctx()};
    const bool    sized{(bits & flags::INT_SIZE) != 0};

    // An unsuffixed character is an untyped integer that defaults to a code point's width
    if (!sized && width == 0) {
        auto& untyped{(bits & flags::INT_CHAR) != 0 ? ctx.get_char_constant()
                                                    : builtin(gs::type_kind::COMPTIME_INT)};
        return intern_int(comp_.constants(), value, untyped);
    }

    auto& type{sized ? builtin(is_signed ? gs::type_kind::ISIZE : gs::type_kind::USIZE)
                     : ctx.get_int(static_cast<u16>(width), is_signed)};
    if (!gs::comptime_int_fits(value, type, comp_.ptr_bits()) &&
        !negated_to_fit(inst, value, type)) {
        return comp_.report(
            u_, src, gs::error::LITERAL_OUT_OF_RANGE, messages::literal_out_of_range(ctx, type));
    }

    return intern_int(comp_.constants(), value, type);
}

auto analyzer::analyze_float_lit(inst_id inst) -> result<operand> {
    auto       rec{record(inst)};
    const auto bits{f128::from_bits(limbs_value(rec.words_field()))};
    const auto width{rec.u32_field()};

    // An untyped literal is kept in quad precision
    const auto node{src_of(inst).as_opt<ast::node_id>()};
    const auto lit{width == 0 && node ? u_.source->ast.get_as_opt<ast::float_literal_expr>(*node)
                                      : stdx::none};
    std::array<f128, 4> narrowed{};
    bool                rounds_twice{false};
    if (lit) {
        for (const auto format :
             {float_format::HALF, float_format::SINGLE, float_format::DOUBLE, float_format::X87}) {
            narrowed.at(std::to_underlying(format)) = lit->value_in(format);
            rounds_twice |= !(narrowed.at(std::to_underlying(format)) == bits.round_to(format));
        }
    }

    auto&      type{[&] -> gs::type& {
        switch (width) {
        case 16:  return builtin(gs::type_kind::F16);
        case 32:  return builtin(gs::type_kind::F32);
        case 64:  return builtin(gs::type_kind::F64);
        case 80:  return builtin(gs::type_kind::F80);
        case 128: return builtin(gs::type_kind::F128);
        default:  return builtin(gs::type_kind::COMPTIME_FLOAT);
        }
    }()};
    const auto index{gir::value::constant(comp_.constants(), bits, type).constant_index()};
    if (lit) {
        comp_.note_literal_digits(
            index, rounds_twice ? stdx::option<std::array<f128, 4>>{narrowed} : stdx::none);
    }
    return index;
}

auto analyzer::analyze_int_type(inst_id inst) -> result<operand> {
    auto       rec{record(inst)};
    const auto bits{rec.u32_field()};
    const auto is_signed{(rec.u32_field() & 1U) != 0};
    return interned_type(comp_.ctx().get_int(static_cast<u16>(bits), is_signed));
}

namespace {

// The mutability a container type's flags spell
[[nodiscard]] auto container_mutability(u32 bits) noexcept -> gs::types::mutability_modifiers {
    if ((bits & flags::POLY) != 0) { return gs::types::mut::POLY; }
    auto mut{(bits & flags::MUT) != 0 ? gs::types::mut::MUTABLE : gs::types::mut::CONSTANT};

    // `mut?` is the enclosing function's mutability, which each call supplies
    if ((bits & flags::POLY) != 0) { mut = gs::types::mut::POLY; }
    if ((bits & flags::VOLATILE) != 0) { mut = mut | gs::types::mut::VOLATILE; }
    return mut;
}

} // namespace

auto analyzer::analyze_elem_type(inst_id inst, tag t) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    auto&       elem{*TRY(resolve_type(*rec.ref_field(), src))};
    const auto  bits{rec.u32_field()};
    // `volatile T` is written like a pointer's modifier but names `T` itself, qualified
    if ((bits & flags::VOLATILE) != 0 && t == tag::ptr_type) {
        return interned_type(*comp_.ctx().pool.with_volatile(elem, true));
    }

    if ((bits & flags::VOLATILE) != 0 && t == tag::ref_type) {
        return comp_.unsupported(u_, src, "a volatile type");
    }
    if (elem.get_kind() == gs::type_kind::INTERFACE) {
        return comp_.report(u_,
                            src,
                            gs::error::INTERFACE_NOT_A_VALUE,
                            fmt::format("'{}' is an interface, not a value type; point at a 'dyn "
                                        "{}' instead",
                                        comp_.ctx().type_display_name(elem),
                                        comp_.ctx().type_display_name(elem)));
    }

    auto&      ctx{comp_.ctx()};
    const auto mut{container_mutability(bits)};

    // `&fn(...)` borrows the same callable that a bare `fn(...)` already is
    const auto elem_inst{as_inst(*record(inst).ref_field())};
    if (t == tag::ref_type && elem_inst && mod().tag_of(*elem_inst) == tag::fn_type) {
        return interned_type(elem);
    }

    switch (t) {
    case tag::ptr_type: return interned_type(ctx.get_pointer(mut, elem));
    case tag::ref_type: return interned_type(ctx.get_reference(mut, elem));
    default: return interned_type(ctx.get_slice(mut, (bits & flags::SENTINEL) != 0, elem));
    }
}

auto analyzer::analyze_fn_type(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  params{rec.refs_field()};
    const auto  names{rec.count()};
    for (u32 i{0}; i < names; ++i) { rec.skip_str(); }
    const auto ret{*rec.ref_field()};
    const auto conv{static_cast<ast::calling_convention>(rec.u32_field())};
    const auto bits{rec.u32_field()};
    const bool is_extern{(bits & flags::EXTERN_FN) != 0};
    // `dyn Fn(...)` is an erased `fn(...)` and an `impl Fn(...)` bound is a copmtime contract
    const bool is_pattern{(bits & flags::IMPL_FN) != 0};

    if (!is_extern && (bits & flags::EXPLICIT_CONV) != 0) {
        return comp_.report(u_,
                            src,
                            gs::error::CALLCONV_REQUIRES_EXTERN_FN,
                            "`callconv(...)` only applies to a thin `extern fn(...)` type; an "
                            "erased `fn(...)` callable has no C calling convention");
    }

    // Neither end of a function type may be inferred or have no size
    const auto check{[&](gs::type& type, const source& at, std::string_view end) -> result<void> {
        if (type.get_kind() == gs::type_kind::AUTO && !is_pattern) {
            return comp_.report(u_,
                                at,
                                gs::error::ILLEGAL_AUTO_USAGE,
                                fmt::format("Function types cannot have 'auto' {}", end));
        }

        if (type.get_kind() == gs::type_kind::OPAQUE) {
            return comp_.report(u_,
                                at,
                                gs::error::ILLEGAL_OPAQUE_TYPE,
                                "An 'opaque' value has no size, so it can't be passed or returned; "
                                "use a pointer to it instead");
        }

        return {};
    }};

    auto& ctx{comp_.ctx()};
    auto  param_types{ctx.pool.get_many_unsafe(params.size())};
    for (usize i{0}; i < params.size(); ++i) {
        const auto& at{as_inst(params[i]) ? src_of(*as_inst(params[i])) : src};
        auto&       type{*TRY(resolve_type(params[i], at))};
        TRY(check(type, at, "parameter types"));
        param_types[i] = &type;
    }

    const auto& ret_src{as_inst(ret) ? src_of(*as_inst(ret)) : src};
    auto&       return_type{*TRY(resolve_type(ret, ret_src))};
    TRY(check(return_type, ret_src, "return type"));
    if (is_extern) {
        const auto triple{codegen::resolve_target_triple(ctx.target_opts.triple_str)};
        if (!codegen::has_c_abi_model(triple, codegen::to_llvm_callconv(conv, triple))) {
            return comp_.unsupported(
                u_, src, "an `extern fn` type on a target without a C ABI model");
        }
    }

    return interned_type(ctx.get_function(
        param_types, return_type, (bits & flags::VARIADIC) != 0, conv, !is_extern));
}

auto analyzer::analyze_array_type(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  len_ref{rec.ref_field()};
    auto&       elem{*TRY(resolve_type(*rec.ref_field(), src))};
    const auto  bits{rec.u32_field()};
    if ((bits & flags::POLY) != 0) { return comp_.unsupported(u_, src, "a `mut?` type"); }
    if (!len_ref) { return comp_.unsupported(u_, src, "an array type sized by its items"); }
    const auto len{TRY(array_length(*len_ref, src))};

    // Past 2^48 bytes no target can address the array, and sizes start overflowing
    constexpr u128 MAX_ARRAY_BYTES{u128{1} << 48U};
    const bool     scalar{gs::is_numeric(elem.get_kind()) || elem.get_kind() == gs::type_kind::BOOL ||
                      elem.get_kind() == gs::type_kind::POINTER};
    const auto     elem_bytes{
        scalar ? gir::size_of(elem, codegen::type_layout::resolve(comp_.ctx().target_opts.triple_str))
                   : usize{1}};
    if (u128{len} > MAX_ARRAY_BYTES || u128{len} * u128{elem_bytes} > MAX_ARRAY_BYTES) {
        return comp_.report(u_, src, gs::error::TYPE_MISMATCH, "Array type is too large");
    }

    return interned_type(comp_.ctx().get_array(
        container_mutability(bits), (bits & flags::SENTINEL) != 0, len, elem));
}

auto analyzer::array_length(ref len_ref, const source& src) -> result<usize> {
    const auto known{[&] -> result<operand> {
        const auto scope{comptime_scope()};
        return reevaluate(len_ref, src);
    }()};
    if (!known && known.error() == stop::RUNTIME) {
        return comp_.report_runtime_need(
            u_,
            stdx::none,
            gs::error::COMPTIME_EVALUATION_FAILED,
            "An array's length must be known at compile time; this one reads a runtime value");
    }

    const auto len{TRY(known)};
    auto&      usize_type{builtin(gs::type_kind::USIZE)};
    const auto coerced{TRY(coerce_to(len, usize_type, src, {.what = use_site::kind::STORE}))};
    const auto value{int_of(comp_.constants().value_at(TRY(require_comptime(coerced, src))))};
    const auto small{value ? value->to_u128() : stdx::none};
    if (!small || *small > static_cast<u128>(std::numeric_limits<usize>::max())) {
        return comp_.unsupported(u_, src, "an array length past 64 bits");
    }

    return static_cast<usize>(*small);
}

auto analyzer::analyze_func(inst_id inst) -> result<operand> {
    if (!declaring_ || !is_comptime()) {
        // A literal in a runtime body is a function of its own, or the body of a closure after it
        if (fn_ && !declaring_) {
            if (wrapping_closure(inst)) { return void_value(); }
            return local_function(inst, {});
        }

        // A literal a top-level statement holds is checked like a function nothing calls
        if (statement_ && !fn_) {
            const auto node{src_of(inst).as_opt<ast::node_id>()};
            auto       member{member_context_here()};
            member.symbol = comp_.lasting_name(
                fmt::format("statement{}", node ? node->get_index() : std::to_underlying(inst)));
            const auto declared{TRY(comp_.declare_function(u_, *statement_, inst, member))};
            if (const auto fn{declared.as_opt<function_ref>()}) { comp_.request_body(*fn->fn); }
            return declared;
        }

        return comp_.unsupported(u_, src_of(inst), "a function literal used as a value");
    }

    // A closure's own instruction declares it, with what it captures
    if (closure_declares(inst)) { return void_value(); }
    return comp_.declare_function(
        u_, *std::exchange(declaring_, stdx::none), inst, member_context_here());
}

} // namespace ghoti::uir::sema
