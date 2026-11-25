#include <algorithm>
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
#include "compiler/ip/index.hh"
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
    if (args.size() != 1) { return comp_.unsupported(u_, src, "a math builtin's arity"); }
    const auto given{TRY(resolve_value(args.front(), src))};
    const auto arg{TRY(read_through_reference(given, src))};
    if (gs::is_vector(type_of(arg))) { return vector_math(which, {arg}, stdx::none, src); }
    return bit_count_of(which, arg, src);
}

auto analyzer::bit_count_of(syntax::builtin_t which, const operand& arg, const source& src)
    -> result<operand> {
    const auto name{syntax::builtin_name(which)};
    auto&      type{type_of(arg)};
    auto&      ctx{comp_.ctx()};
    const auto kind{type.get_kind()};
    if (gs::is_comptime_numeric(kind)) {
        return comp_.unsupported(u_, src, fmt::format("`{}` of an untyped value", name));
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
    if (gs::is_vector(type_of(arg))) {
        return vector_math(syntax::builtin_t::ABS, {arg}, stdx::none, src);
    }

    return abs_of(arg, src);
}

auto analyzer::abs_of(const operand& arg, const source& src) -> result<operand> {
    auto& type{type_of(arg)};
    auto& pool{comp_.constants()};
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
        runtime_safety() && gs::is_integer(type.get_kind()) &&
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
    if (args.size() != 2) { return comp_.unsupported(u_, src, "a math builtin's arity"); }
    const auto lhs_given{TRY(resolve_value(args[0], src))};
    const auto rhs_given{TRY(resolve_value(args[1], src))};
    const auto lhs{TRY(read_through_reference(lhs_given, src))};
    const auto rhs{TRY(read_through_reference(rhs_given, src))};
    if (gs::is_vector(type_of(lhs)) || gs::is_vector(type_of(rhs))) {
        return vector_math(which, {lhs, rhs}, stdx::none, src);
    }

    return binary_math_of(which, lhs, rhs, src);
}

auto analyzer::binary_math_of(syntax::builtin_t which,
                              const operand&    lhs,
                              const operand&    rhs,
                              const source&     src) -> result<operand> {
    const auto name{syntax::builtin_name(which)};
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
    const bool checked{runtime_safety() && gs::is_integer(work.get_kind()) && int_op &&
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
    if (args.size() != 1) { return comp_.unsupported(u_, src, "a math builtin's arity"); }
    const auto given{TRY(resolve_value(args.front(), src))};
    const auto arg{TRY(read_through_reference(given, src))};
    if (gs::is_vector(type_of(arg))) { return vector_math(which, {arg}, stdx::none, src); }
    return float_math_of(which, arg, src);
}

auto analyzer::float_math_of(syntax::builtin_t which, const operand& arg, const source& src)
    -> result<operand> {
    const auto name{syntax::builtin_name(which)};
    auto&      type{type_of(arg)};

    // An untyped operand computes exactly, and a concrete float it meets later computes it again
    if (const auto known{comptime_value(arg)}; known && gs::is_comptime_numeric(type.get_kind())) {
        const auto& value{comp_.constants().value_at(*known)};
        const auto  exact{int_of(value)};
        const auto  number{value.data.as_opt<f128>()};
        const auto  small{exact ? exact->to_i128() : stdx::none};
        if (!number && !small) {
            return comp_.unsupported(u_, src, fmt::format("`{}` of this value", name));
        }
        const f128 input{number ? *number
                                : gir::semantics::float_from_int({.bits = 128, .is_signed = true},
                                                                 static_cast<u128>(*small),
                                                                 float_format::QUAD)};
        const auto function{*semantics::math_function_of(which)};
        const auto folded{evaluate_traced(function, input, float_format::QUAD)};
        if (folded.overflowed) {
            return comp_.report(
                u_,
                src,
                gs::error::LITERAL_OUT_OF_RANGE,
                fmt::format("'{}' of {} is out of range for type 'f128'", name, input));
        }

        auto&      untyped{builtin(gs::type_kind::COMPTIME_FLOAT)};
        const auto result{comp_.constants().intern(folded.value, untyped)};
        const bool exact_enough{function == math_function::SQRT ||
                                function == math_function::FLOOR ||
                                function == math_function::CEIL};
        if (!exact_enough) {
            comp_.note_math_origin(result, {.function = function, .input = input});
        }
        return operand{result};
    }

    if (!gs::is_float(type.get_kind())) {
        return comp_.unsupported(
            u_, src, fmt::format("`{}` of something other than a typed float", name));
    }

    const auto function{*semantics::math_function_of(which)};
    if (const auto known{comptime_value(arg)}) {
        // A typed operand's result is rounded once, into its own format
        const auto input{comp_.constants().value_at(*known).data.as_opt<f128>()};
        const auto format{gs::float_format_of(type)};
        if (!input || !format) {
            return comp_.unsupported(u_, src, fmt::format("`{}` folded at compile time", name));
        }

        const f128 x{*input};
        const auto folded{evaluate_traced(function, x, *format)};
        if (folded.overflowed) {
            return comp_.report(u_,
                                src,
                                gs::error::LITERAL_OUT_OF_RANGE,
                                fmt::format("'{}' of {} is out of range for type '{}'",
                                            name,
                                            x,
                                            comp_.ctx().type_display_name(type)));
        }
        return operand{comp_.constants().intern(folded.value, type)};
    }

    TRY(require_runtime(src));
    std::vector<gir::value> call_args;
    call_args.emplace_back(TRY(as_runtime(arg, src)));
    const auto result{builder_.emit_builtin_call(name, std::move(call_args), type)};
    return operand{gir::value{*result, type}};
}

auto analyzer::builtin_mul_add(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (args.size() != 4) { return comp_.unsupported(u_, src, "a math builtin's arity"); }
    auto&                type{*TRY(resolve_type(args[0], src))};
    std::vector<operand> values;
    for (const auto arg : args.subspan(1)) {
        const auto given{TRY(resolve_value(arg, src))};
        values.emplace_back(TRY(read_through_reference(given, src)));
    }

    if (gs::is_vector(type) ||
        std::ranges::any_of(values, [&](const operand& v) { return gs::is_vector(type_of(v)); })) {
        return vector_math(syntax::builtin_t::MUL_ADD, std::move(values), type, src);
    }

    return mul_add_of(type, values, src);
}

auto analyzer::mul_add_of(gs::type& type, gsl::span<const operand> values, const source& src)
    -> result<operand> {
    auto& ctx{comp_.ctx()};
    auto& pool{comp_.constants()};
    if (!gs::is_numeric(type.get_kind()) || gs::is_comptime_numeric(type.get_kind())) {
        return comp_.unsupported(u_, src, "a `@mulAdd` of this type");
    }

    // Every operand is a `T`
    std::vector<operand> typed;
    for (const auto& value : values) {
        auto& from{type_of(value)};
        if (!is_untyped_literal(value) && !gs::is_comptime_numeric(from.get_kind()) &&
            !gs::is_assignable(from, type)) {
            return comp_.report(u_,
                                src,
                                gs::error::OPERATOR_TYPE_MISMATCH,
                                fmt::format("'@mulAdd' operands must be '{}'; found '{}'",
                                            ctx.type_display_name(type),
                                            ctx.type_display_name(from)));
        }

        typed.emplace_back(TRY(coerce_to(value, type, src, {.what = use_site::kind::STORE})));
    }

    // Known operands fold with one rounding, or as `a * b + c` for integers
    const auto a{comptime_value(typed[0])};
    const auto b{comptime_value(typed[1])};
    const auto c{comptime_value(typed[2])};
    if (a && b && c) {
        if (const auto format{gs::float_format_of(type)}) {
            const auto fa{pool.value_at(*a).data.as_opt<f128>()};
            const auto fb{pool.value_at(*b).data.as_opt<f128>()};
            const auto fc{pool.value_at(*c).data.as_opt<f128>()};
            if (!fa || !fb || !fc) {
                return comp_.unsupported(u_, src, "`@mulAdd` of these values");
            }
            const f128 x{*fa};
            const f128 y{*fb};
            const f128 z{*fc};
            return operand{pool.intern(fused_multiply_add(x, y, z, *format), type)};
        }

        const binary_info mul{.kind = gir::instruction_kind::MUL, .cls = op_class::ARITHMETIC};
        const binary_info add{.kind = gir::instruction_kind::ADD, .cls = op_class::ARITHMETIC};
        if (const auto product{TRY(fold_binary(mul, type, typed[0], typed[1], src))}) {
            if (const auto sum{TRY(fold_binary(add, type, *product, typed[2], src))}) {
                return *sum;
            }
        }
    }

    TRY(require_runtime(src));
    std::vector<gir::value> call_args;
    for (const auto& value : typed) { call_args.emplace_back(TRY(as_runtime(value, src))); }
    const auto result{builder_.emit_builtin_call("@mulAdd", std::move(call_args), type)};
    return operand{gir::value{*result, type}};
}

auto analyzer::math_lane(syntax::builtin_t        which,
                         gsl::span<const operand> lanes,
                         gs::type&                lane,
                         const source&            src) -> result<operand> {
    using syntax::builtin_t;
    switch (which) {
    case builtin_t::CLZ:
    case builtin_t::CTZ:
    case builtin_t::POPCOUNT:  return bit_count_of(which, lanes[0], src);
    case builtin_t::ABS:       return abs_of(lanes[0], src);
    case builtin_t::MUL_ADD:   return mul_add_of(lane, lanes, src);
    case builtin_t::MIN:
    case builtin_t::MAX:
    case builtin_t::DIV_TRUNC:
    case builtin_t::DIV_FLOOR:
    case builtin_t::DIV_EXACT:
    case builtin_t::REM:
    case builtin_t::MOD:       return binary_math_of(which, lanes[0], lanes[1], src);
    default:                   return float_math_of(which, lanes[0], src);
    }
}

auto analyzer::vector_math(syntax::builtin_t       which,
                           std::vector<operand>    values,
                           stdx::option<gs::type&> declared,
                           const source&           src) -> result<operand> {
    using syntax::builtin_t;
    auto&      ctx{comp_.ctx()};
    auto&      pool{comp_.constants()};
    const auto name{syntax::builtin_name(which)};
    const auto fail{[&](std::string message) {
        return comp_.report(
            u_, src, gs::error::TYPE_MISMATCH, fmt::format("'{}': {}", name, std::move(message)));
    }};

    // The vector every operand takes the lanes of, a scalar broadcasting to them
    auto&      shape{[&]() -> gs::type& {
        if (declared && gs::is_vector(*declared)) { return *declared; }
        for (const auto& v : values) {
            if (gs::is_vector(type_of(v))) { return type_of(v); }
        }

        return *declared;
    }()};
    const auto vec{*gs::vector_of(shape)};
    auto&      lane{vec.underlying};
    for (auto& v : values) {
        auto& t{type_of(v)};
        if (gs::is_vector(t)) {
            if (gs::vector_of(t)->len != vec.len ||
                !gs::is_same_unqualified(gs::lane_type_of(t), lane)) {
                return fail(fmt::format("expects a '{}'; found '{}'",
                                        ctx.type_display_name(shape),
                                        ctx.type_display_name(t)));
            }
        }
    }

    const bool is_int{gs::is_integer(lane.get_kind())};
    const bool is_float{gs::is_float(lane.get_kind())};
    const auto lanes_must_be{[&](bool fits, std::string_view what) -> result<void> {
        if (fits) { return {}; }
        return fail(
            fmt::format("expects {} lanes; found '{}'", what, ctx.type_display_name(shape)));
    }};
    auto&      same_shape{ctx.get_vector(gs::types::mut::CONSTANT, vec.len, ctx.as_lane(lane))};
    auto*      result_type{&same_shape};
    switch (which) {
    case builtin_t::CLZ:
    case builtin_t::CTZ:
    case builtin_t::POPCOUNT: {
        TRY(lanes_must_be(is_int, "integer"));
        const auto bits{std::bit_width(u64{domain_of(lane).bits})};
        result_type = &ctx.get_vector(
            gs::types::mut::CONSTANT, vec.len, ctx.get_int(static_cast<u16>(bits), false));
        break;
    }
    case builtin_t::ABS:
    case builtin_t::MIN:
    case builtin_t::MAX:
    case builtin_t::MUL_ADD:   TRY(lanes_must_be(is_int || is_float, "integer or float")); break;
    case builtin_t::DIV_TRUNC:
    case builtin_t::DIV_FLOOR:
    case builtin_t::DIV_EXACT:
    case builtin_t::REM:
    case builtin_t::MOD:       TRY(lanes_must_be(is_int, "integer")); break;
    default:                   TRY(lanes_must_be(is_float, "float")); break;
    }

    // Every operand is the vector itself, a scalar operand splat into its lanes
    std::vector<operand> widened;
    for (const auto& v : values) {
        widened.emplace_back(TRY(vector_operand(v, lane, vec.len, src)));
    }

    // Known lanes fold one at a time, otherwise the whole call runs at runtime
    std::vector<std::vector<ip::index>> known;
    for (const auto& w : widened) {
        if (const auto lanes{lanes_of(w, vec.len)}) { known.emplace_back(*lanes); }
    }

    const auto fold_lane{[&](u32 i) -> result<stdx::option<ip::index>> {
        std::vector<operand> lane_values;
        for (const auto& k : known) { lane_values.emplace_back(k[i]); }
        const auto scope{comptime_scope()};
        return comptime_value(TRY(math_lane(which, lane_values, lane, src)));
    }};
    if (known.size() == widened.size() && is_comptime()) {
        std::vector<stdx::option<ip::index>> out;
        for (u32 i{0}; i < vec.len; ++i) {
            const auto index{TRY(fold_lane(i))};
            if (!index) {
                return comp_.unsupported(u_, src, fmt::format("`{}` of these lanes", name));
            }
            out.emplace_back(*index);
        }

        return operand{pool.intern(ip::aggregate_value{std::move(out)}, *result_type)};
    }

    if (known.size() == widened.size()) {
        const auto                           before{comp_.speculate()};
        std::vector<stdx::option<ip::index>> out;
        for (u32 i{0}; i < vec.len; ++i) {
            const auto index{fold_lane(i)};
            if (!index || !*index) { break; }
            out.emplace_back(**index);
        }

        if (out.size() == vec.len) {
            return operand{pool.intern(ip::aggregate_value{std::move(out)}, *result_type)};
        }
        comp_.abandon(before, false);
    }

    TRY(require_runtime(src));
    std::vector<gir::value> call_args;
    for (usize i{0}; i < values.size(); ++i) {
        call_args.emplace_back(TRY(runtime_lanes(values[i], widened[i], src)));
    }

    // Division by zero, `MIN / -1` and `@abs(MIN)` panic in any lane like the operators they mirror
    const auto int_op{semantics::int_op_of(which)};
    const bool signed_lane{gs::is_signed_integer(lane)};
    const bool checked{runtime_safety() && is_int &&
                       ((int_op && semantics::any(semantics::runtime_checks(
                                       *int_op, {.bits = 1, .is_signed = signed_lane}))) ||
                        (which == builtin_t::ABS && semantics::any(semantics::runtime_checks(
                                                        semantics::int_unary_op::ABS,
                                                        {.bits = 1, .is_signed = signed_lane}))))};
    const auto result{builder_.emit_builtin_call(
        name, std::move(call_args), *result_type, stdx::none, stdx::none, stdx::none, checked)};
    return operand{gir::value{*result, *result_type}};
}

auto analyzer::builtin_overflow(syntax::builtin_t    which,
                                gsl::span<const ref> args,
                                const source&        src) -> result<operand> {
    const auto name{syntax::builtin_name(which)};
    if (args.size() != 3) { return comp_.unsupported(u_, src, "an overflow builtin's arity"); }
    auto&      ctx{comp_.ctx()};
    const auto display{[&](const gs::type& t) { return ctx.type_display_name(t); }};
    const auto lhs_given{TRY(resolve_value(args[0], src))};
    const auto rhs_given{TRY(resolve_value(args[1], src))};
    const auto lhs{TRY(read_through_reference(lhs_given, src))};
    const auto rhs{TRY(read_through_reference(rhs_given, src))};
    const auto slot{TRY(resolve_value(args[2], src))};
    auto&      lhs_type{type_of(lhs)};
    auto&      rhs_type{type_of(rhs)};
    auto&      slot_type{type_of(slot)};
    const auto out{gs::pointee_of(slot_type)};
    const bool is_shift{which == syntax::builtin_t::SHL_WITH_OVERFLOW};

    // A vector's lanes each overflow on their own, a scalar operand taking every lane
    const auto vec{gs::vector_of(lhs_type) ? gs::vector_of(lhs_type) : gs::vector_of(rhs_type)};
    if (vec) {
        auto& lane{vec->underlying};
        auto& shape{ctx.get_vector(gs::types::mut::CONSTANT, vec->len, ctx.as_lane(lane))};
        if (!gs::is_integer(lane.get_kind())) {
            return comp_.report(
                u_,
                src,
                gs::error::TYPE_MISMATCH,
                fmt::format("'{}': expects integer lanes; found '{}'",
                            name,
                            display(gs::vector_of(lhs_type) ? lhs_type : rhs_type)));
        }

        if (!out || slot_type.is_constant() || !gs::is_same_unqualified(*out, shape)) {
            return comp_.report(u_,
                                arg_src(src, 2).value_or(src),
                                gs::error::TYPE_MISMATCH,
                                fmt::format("'{}': expects its third argument to be a '&mut {}' "
                                            "result reference; found '{}'",
                                            name,
                                            display(shape),
                                            display(slot_type)));
        }

        TRY(require_runtime(src));
        const auto              a{TRY(vector_operand(lhs, lane, vec->len, src))};
        const auto              b{TRY(vector_operand(rhs, lane, vec->len, src))};
        std::vector<gir::value> call_args;
        call_args.emplace_back(TRY(runtime_lanes(lhs, a, src)));
        call_args.emplace_back(TRY(runtime_lanes(rhs, b, src)));
        call_args.emplace_back(TRY(as_runtime(slot, src)));
        auto&      result_type{ctx.with_lane(shape, builtin(gs::type_kind::BOOL))};
        const auto result{builder_.emit_builtin_call(name, std::move(call_args), result_type)};
        return operand{gir::value{*result, result_type}};
    }

    // A shift's count is not a peer of the value shifted, the other operands meet at their peer
    const auto int_like{[](const gs::type& t) { return is_int_like(t); }};
    if (!int_like(lhs_type) || !int_like(rhs_type)) {
        return comp_.report(u_,
                            src,
                            gs::error::OPERATOR_TYPE_MISMATCH,
                            fmt::format("'{}' expects two integer operands; found '{}' and '{}'",
                                        name,
                                        display(lhs_type),
                                        display(rhs_type)));
    }

    const std::array operands{lhs, rhs};
    auto&            peer{is_shift ? lhs_type : *TRY(peer_type(operands, src))};
    const bool       untyped{peer.get_kind() == gs::type_kind::COMPTIME_INT};
    const bool       slot_fits{
        out && (untyped ? gs::is_integer(out->get_kind()) : gs::is_same_unqualified(*out, peer))};
    if (!slot_fits || slot_type.is_constant()) {
        return comp_.report(u_,
                            arg_src(src, 2).value_or(src),
                            gs::error::TYPE_MISMATCH,
                            fmt::format("'{}' expects its third argument to be a '&mut {}' result "
                                        "reference; found '{}'",
                                        name,
                                        display(ctx.default_concrete(peer)),
                                        display(slot_type)));
    }

    // Both operands become the result's type, except a typed shift count
    TRY(require_runtime(src));
    const bool              untyped_count{rhs_type.get_kind() == gs::type_kind::COMPTIME_INT};
    const auto              a{TRY(coerce_to(lhs, *out, src, {.what = use_site::kind::STORE}))};
    const auto              b{!is_shift || untyped_count
                                  ? TRY(coerce_to(rhs, *out, src, {.what = use_site::kind::STORE}))
                                  : rhs};
    std::vector<gir::value> call_args;
    call_args.emplace_back(TRY(as_runtime(a, src)));
    call_args.emplace_back(TRY(as_runtime(b, src)));
    call_args.emplace_back(TRY(as_runtime(slot, src)));
    auto&      bool_type{builtin(gs::type_kind::BOOL)};
    const auto result{builder_.emit_builtin_call(name, std::move(call_args), bool_type)};
    return operand{gir::value{*result, bool_type}};
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
    return mem_view{
        .data  = data,
        .count = count,
        .bytes = is_dest ? stdx::option<gir::value>{byte_count(
                               count, gir::size_of(slice->underlying, layout))}
                         : stdx::none,
    };
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
    if (runtime_safety()) {
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
