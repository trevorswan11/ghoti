#include <stdx/option.hh>

#include "compiler/sema/type.hh"
#include "internal.hh"
#include "support/big_int.hh"
#include "support/float128.hh"

namespace ghoti::uir::sema {

namespace {

// Two's-complement `u32` limbs, least significant first, as one `u128`
[[nodiscard]] auto limbs_value(gsl::span<const u32> limbs) noexcept -> u128 {
    u128 value{0};
    for (usize i{0}; i < limbs.size() && i < 4; ++i) {
        value |= static_cast<u128>(limbs[i]) << (32U * i);
    }
    return value;
}

} // namespace

auto intern_int(ip::pool& pool, const big_int& value, gs::type& type) -> ip::index {
    if (const auto small{value.to_i128()}) {
        if (*small >= static_cast<i128>(INT64_MIN) && *small <= static_cast<i128>(INT64_MAX)) {
            return pool.intern(static_cast<i64>(*small), type);
        }
        if (*small >= 0 && *small <= static_cast<i128>(UINT64_MAX)) {
            return pool.intern(static_cast<u64>(*small), type);
        }
        return pool.intern(*small, type);
    }
    if (const auto wide{value.to_u128()}) { return pool.intern(*wide, type); }
    return pool.intern(value, type);
}

auto analyzer::analyze_int_value(inst_id inst) -> result<operand> {
    const big_int value{static_cast<u128>(mod().data_of(inst).int_value)};
    return intern_int(comp_.constants(), value, builtin(gs::type_kind::COMPTIME_INT));
}

// A suffixed literal one past its type's maximum, negated right away: `-128i8`
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
    auto        rec{record(inst)};
    const auto  limbs{rec.words_field()};
    const auto  width{rec.u32_field()};
    const auto  bits{rec.u32_field()};
    const auto& src{src_of(inst)};
    if ((bits & (flags::INT_BIG | flags::INT_CHAR)) != 0) {
        return comp_.unsupported(u_, src, "a character or wider-than-128-bit literal");
    }
    const big_int value{limbs_value(limbs)};
    const bool    is_signed{(bits & flags::INT_SIGNED) != 0};
    auto&         ctx{comp_.ctx()};
    const bool    sized{(bits & flags::INT_SIZE) != 0};
    if (!sized && width == 0) {
        return intern_int(comp_.constants(), value, builtin(gs::type_kind::COMPTIME_INT));
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
    return gir::value::constant(comp_.constants(), bits, type).constant_index();
}

auto analyzer::analyze_int_type(inst_id inst) -> result<operand> {
    auto       rec{record(inst)};
    const auto bits{rec.u32_field()};
    const auto is_signed{(rec.u32_field() & 1U) != 0};
    return interned_type(comp_.ctx().get_int(static_cast<u16>(bits), is_signed));
}

auto analyzer::analyze_func(inst_id inst) -> result<operand> {
    if (!declaring_ || !is_comptime()) {
        return comp_.unsupported(u_, src_of(inst), "a function literal used as a value");
    }
    return comp_.declare_function(u_, *std::exchange(declaring_, stdx::none), inst);
}

} // namespace ghoti::uir::sema
