#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/gir/semantics.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "gsl/span"
#include "internal.hh"
#include "support/big_int.hh"

namespace ghoti::uir::sema {

auto analyzer::vector_at_runtime(const source& src) -> result<void> {
    TRY(require_runtime(src));
    return comp_.unsupported(u_, src, "a vector at runtime");
}

auto analyzer::vector_read(gs::type& type) -> stdx::option<gs::type&> {
    const auto pointee{type.get_kind() == gs::type_kind::REFERENCE ? gs::pointee_of(type)
                                                                   : stdx::none};
    auto&      read{pointee ? *pointee : type};
    return gs::is_vector(read) ? stdx::option<gs::type&>{read} : stdx::none;
}

auto analyzer::lanes_of(const operand& op, u32 len) -> stdx::option<std::vector<ip::index>> {
    const auto known{comptime_value(op)};
    if (!known) { return stdx::none; }
    const auto& data{comp_.constants().value_at(*known).data};
    const auto  agg{data.as_opt<ip::aggregate_value>()};
    if (!agg || agg->elements.size() != len) { return stdx::none; }
    std::vector<ip::index> lanes;
    for (const auto element : agg->elements) {
        if (!element) { return stdx::none; }
        lanes.emplace_back(*element);
    }
    return lanes;
}

auto analyzer::splat(const operand& scalar, gs::type& vector, const source& src)
    -> result<operand> {
    const auto& vec{*gs::vector_of(vector)};
    const auto  lane{TRY(coerce_to(scalar, vec.underlying, src, {.what = use_site::kind::STORE}))};
    if (const auto known{comptime_value(lane)}) {
        return operand{comp_.constants().intern(
            ip::aggregate_value{std::vector<stdx::option<ip::index>>(vec.len, *known)}, vector)};
    }

    TRY(vector_at_runtime(src));
    const auto value{TRY(as_runtime(lane, src))};
    return operand{
        gir::value{builder_.emit_vector_op(gir::instruction_kind::SPLAT, {value}, vector), vector}};
}

auto analyzer::vector_operand(const operand& op, gs::type& lane, u32 len, const source& src)
    -> result<operand> {
    auto&      shape{comp_.ctx().get_vector(gs::types::mut::CONSTANT, len, lane)};
    const auto read{TRY(read_through_reference(op, src))};
    if (vector_read(type_of(read))) {
        return coerce_to(read, shape, src, {.what = use_site::kind::STORE});
    }

    return splat(read, shape, src);
}

auto analyzer::coerce_vector(const operand&  op,
                             const gs::type& from,
                             gs::type&       target,
                             const source&   src) -> result<stdx::option<operand>> {
    auto&      pool{comp_.constants()};
    const auto to_vec{gs::vector_of(target)};
    const auto from_vec{gs::vector_of(from)};

    // A list of lanes becomes the vector holding them
    if (const auto init{op.as_opt<anon_init>()}; init && to_vec) {
        if (mod().tag_of(init->inst) != tag::init_array) { return stdx::option<operand>{}; }
        auto rec{record(init->inst)};
        rec.skip_ref();
        const auto lanes{rec.refs_field()};
        if (lanes.size() != to_vec->len) {
            return comp_.report(u_,
                                src,
                                gs::error::TYPE_MISMATCH,
                                fmt::format("A vector of {} lanes needs {} values; found {}",
                                            to_vec->len,
                                            to_vec->len,
                                            lanes.size()));
        }

        std::vector<operand> values;
        bool                 known{true};
        for (const auto lane : lanes) {
            const auto given{TRY(resolve_value(lane, src))};
            values.emplace_back(
                TRY(coerce_to(given, to_vec->underlying, src, {.what = use_site::kind::STORE})));
            known = known && comptime_value(values.back()).has_value();
        }

        if (known) {
            std::vector<stdx::option<ip::index>> elements;
            for (const auto& value : values) { elements.emplace_back(*comptime_value(value)); }
            return stdx::option<operand>{
                operand{pool.intern(ip::aggregate_value{std::move(elements)}, target)}};
        }

        // Each lane is inserted into the vector in turn
        TRY(vector_at_runtime(src));
        gir::value vec{constant(pool.intern(ip::undefined_value{}, target), target)};
        for (usize i{0}; i < values.size(); ++i) {
            const auto lane{TRY(as_runtime(values[i], src))};
            vec = gir::value{builder_.emit_vector_op(gir::instruction_kind::INSERT_LANE,
                                                     {vec, usize_constant(i), lane},
                                                     target),
                             target};
        }
        return stdx::option<operand>{operand{vec}};
    }

    // Vectors of one shape differ only in their qualifiers
    if (to_vec && from_vec) {
        if (to_vec->len != from_vec->len ||
            !gs::is_same_unqualified(to_vec->underlying, from_vec->underlying)) {
            return stdx::option<operand>{};
        }

        if (const auto known{comptime_value(op)}) {
            const auto data{pool.value_at(*known).data};
            return stdx::option<operand>{operand{pool.intern(data, target)}};
        }

        TRY(vector_at_runtime(src));
        const auto value{TRY(as_runtime(op, src))};
        return stdx::option<operand>{operand{gir::value{value.data, target}}};
    }

    // An array and a vector of the same lanes convert into each other by value
    if (gs::is_array_vector_pair(from, target)) {
        const auto slot{op.as_opt<slot_value>()};
        if (const auto known{slot && slot->known ? slot->known : comptime_value(op)}) {
            const auto data{pool.value_at(*known).data};
            return stdx::option<operand>{operand{pool.intern(data, target)}};
        }

        TRY(vector_at_runtime(src));
        const auto value{TRY(as_runtime(op, src))};
        return stdx::option<operand>{operand{gir::value{
            builder_.emit_vector_op(gir::instruction_kind::REPACK, {value}, target), target}}};
    }

    return stdx::option<operand>{};
}

auto analyzer::vector_binary(const binary_info& info,
                             const operand&     lhs,
                             const operand&     rhs,
                             const source&      src) -> result<operand> {
    auto&      ctx{comp_.ctx()};
    auto&      pool{comp_.constants()};
    const auto lhs_shape{vector_read(type_of(lhs))};
    const auto rhs_shape{vector_read(type_of(rhs))};
    const auto lhs_vec{lhs_shape ? gs::vector_of(*lhs_shape) : stdx::none};
    const auto rhs_vec{rhs_shape ? gs::vector_of(*rhs_shape) : stdx::none};
    if (lhs_vec && rhs_vec &&
        (lhs_vec->len != rhs_vec->len ||
         !gs::is_same_unqualified(lhs_vec->underlying, rhs_vec->underlying))) {
        return comp_.report(u_,
                            src,
                            gs::error::OPERATOR_TYPE_MISMATCH,
                            fmt::format("Vector operands need the same lanes; found '{}' and '{}'",
                                        ctx.type_display_name(type_of(lhs)),
                                        ctx.type_display_name(type_of(rhs))));
    }

    // A scalar takes the vector side's lanes
    const auto len{lhs_vec ? lhs_vec->len : rhs_vec->len};
    auto&      lane{lhs_vec ? lhs_vec->underlying : rhs_vec->underlying};
    const auto l{TRY(vector_operand(lhs, lane, len, src))};
    const auto r{TRY(vector_operand(rhs, lane, len, src))};
    const bool is_comparison{info.cls == op_class::EQUALITY || info.cls == op_class::ORDERING};
    auto&      shape{ctx.get_vector(gs::types::mut::CONSTANT, len, lane)};
    auto& result_type{is_comparison ? ctx.with_lane(shape, builtin(gs::type_kind::BOOL)) : shape};

    // Known lanes fold one at a time
    const auto known_l{lanes_of(l, len)};
    const auto known_r{lanes_of(r, len)};
    if (known_l && known_r) {
        std::vector<stdx::option<ip::index>> elements;
        for (u32 i{0}; i < len; ++i) {
            const auto folded{
                TRY(fold_binary(info, lane, operand{(*known_l)[i]}, operand{(*known_r)[i]}, src))};
            const auto value{folded ? comptime_value(*folded) : stdx::none};
            if (!value) { return comp_.unsupported(u_, src, "a vector lane that doesn't fold"); }
            elements.emplace_back(*value);
        }

        return operand{pool.intern(ip::aggregate_value{std::move(elements)}, result_type)};
    }

    TRY(vector_at_runtime(src));
    const auto lv{TRY(as_runtime(l, src))};
    const auto rv{TRY(as_runtime(r, src))};
    if (is_comparison) {
        return gir::value{builder_.emit_binary(info.kind, lv, rv, result_type), result_type};
    }

    const auto op{gir::semantics::int_op_of(info.kind, info.wrapping, info.saturating)};
    const bool checked{op && checks_overflow(*op, lane)};
    return gir::value{
        builder_.emit_binary(info.kind, lv, rv, result_type, checked, info.saturating),
        result_type};
}

auto analyzer::vector_lane(const operand& vector, ref index_ref, const source& src)
    -> result<operand> {
    const auto  read{TRY(read_through_reference(vector, src))};
    auto&       vec_type{type_of(read)};
    const auto& vec{*gs::vector_of(vec_type)};
    const auto  index{TRY(resolve_value(index_ref, src))};
    auto&       usize_type{builtin(gs::type_kind::USIZE)};
    const auto  at{TRY(coerce_to(index, usize_type, src, {.what = use_site::kind::STORE}))};

    // A known lane of a known vector is that lane at compile time
    const auto known_at{comptime_value(at)};
    if (const auto lanes{lanes_of(read, vec.len)}; lanes && known_at && is_comptime()) {
        const auto i{int_of(comp_.constants().value_at(*known_at))};
        const auto small{i ? i->to_u128() : stdx::none};
        if (!small || *small >= vec.len) {
            return comp_.report(
                u_, src, gs::error::COMPTIME_EVALUATION_FAILED, "index out of bounds");
        }

        return operand{(*lanes)[static_cast<usize>(*small)]};
    }

    TRY(vector_at_runtime(src));
    const auto vec_value{TRY(as_runtime(read, src))};
    const auto idx{TRY(as_runtime(at, src))};
    if (comp_.ctx().runtime_safety && !known_at) {
        auto&      bool_type{builtin(gs::type_kind::BOOL)};
        const auto in_bounds{builder_.emit_binary(
            gir::instruction_kind::LT, idx, usize_constant(vec.len), bool_type)};
        TRY(guard(gir::value{in_bounds, bool_type}, "index out of bounds", src));
    }

    auto& lane{vec.underlying};
    return operand{gir::value{
        builder_.emit_vector_op(gir::instruction_kind::EXTRACT_LANE, {vec_value, idx}, lane),
        lane}};
}

auto analyzer::builtin_reduce(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (args.size() != 2) { return comp_.unsupported(u_, src, "a `@reduce` of this arity"); }
    auto&      op_type{*TRY(comp_.prelude_type("ReduceOp", u_, src))};
    const auto op_given{TRY(resolve_value(args[0], src))};
    const auto op{TRY(coerce_to(op_given, op_type, src, {.what = use_site::kind::STORE}))};
    const auto op_index{comptime_value(op)};
    const auto op_name{op_index ? variant_name(*op_index) : stdx::none};
    if (!op_name) {
        return comp_.report(u_,
                            src,
                            gs::error::COMPTIME_EVALUATION_FAILED,
                            "'@reduce' needs an operation known at compile time");
    }

    const auto given{TRY(resolve_value(args[1], src))};
    const auto vector{TRY(read_through_reference(given, src))};
    auto&      vec_type{type_of(vector)};
    const auto vec{gs::vector_of(vec_type)};
    if (!vec) {
        return comp_.report(u_,
                            src,
                            gs::error::TYPE_MISMATCH,
                            fmt::format("'@reduce' needs a vector; found '{}'",
                                        comp_.ctx().type_display_name(vec_type)));
    }

    auto&      lane{vec->underlying};
    const auto names{op_type.get_data().as<gs::types::enum_t>().variant_names};
    const auto ordinal{static_cast<u8>(std::ranges::find(names, *op_name) - names.begin())};

    // Known lanes fold left to right
    if (const auto lanes{lanes_of(vector, vec->len)}) {
        const auto kind{[&]() -> stdx::option<gir::instruction_kind> {
            if (*op_name == "add") { return gir::instruction_kind::ADD; }
            if (*op_name == "mul") { return gir::instruction_kind::MUL; }
            if (*op_name == "and") { return gir::instruction_kind::AND; }
            if (*op_name == "or") { return gir::instruction_kind::OR; }
            if (*op_name == "xor") { return gir::instruction_kind::XOR; }
            return stdx::none;
        }()};
        auto&      pool{comp_.constants()};
        ip::index  folded{lanes->front()};
        for (usize i{1}; i < lanes->size(); ++i) {
            const auto next{(*lanes)[i]};
            if (kind) {
                // A reduction wraps, so a lane sum that overflows keeps its low bits
                const bool        bitwise{*kind == gir::instruction_kind::AND ||
                                          *kind == gir::instruction_kind::OR ||
                                          *kind == gir::instruction_kind::XOR};
                const binary_info info{.kind = *kind,
                                       .cls  = bitwise ? op_class::BITWISE : op_class::ARITHMETIC,
                                       .wrapping = true};
                const auto value{TRY(fold_binary(info, lane, operand{folded}, operand{next}, src))};
                const auto known{value ? comptime_value(*value) : stdx::none};
                if (!known) {
                    return comp_.unsupported(u_, src, "a `@reduce` lane that doesn't fold");
                }
                folded = *known;
                continue;
            }

            // `min` and `max` keep the lane that compares smaller or larger
            const binary_info less{.kind = gir::instruction_kind::LT, .cls = op_class::ORDERING};
            const auto compared{TRY(fold_binary(less, lane, operand{next}, operand{folded}, src))};
            const auto known{compared ? comptime_value(*compared) : stdx::none};
            const auto is_less{known ? pool.value_at(*known).data.as_opt<bool>() : stdx::none};
            if (!is_less) {
                return comp_.unsupported(u_, src, "a `@reduce` lane that doesn't fold");
            }
            if (*is_less == (*op_name == "min")) { folded = next; }
        }

        return operand{folded};
    }

    TRY(vector_at_runtime(src));
    const auto value{TRY(as_runtime(vector, src))};
    return operand{gir::value{
        builder_.emit_vector_op(gir::instruction_kind::REDUCE, {value}, lane, ordinal), lane}};
}

auto analyzer::builtin_select(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (args.size() != 3) { return comp_.unsupported(u_, src, "a `@select` of this arity"); }
    const auto pred_given{TRY(resolve_value(args[0], src))};
    const auto pred{TRY(read_through_reference(pred_given, src))};
    const auto pred_vec{gs::vector_of(type_of(pred))};
    if (!pred_vec || pred_vec->underlying.get_kind() != gs::type_kind::BOOL) {
        return comp_.report(
            u_, src, gs::error::TYPE_MISMATCH, "'@select' needs a vector of bool as its predicate");
    }

    // The lane type is the first vector branch's, a scalar branch taking every lane
    const auto a{TRY(resolve_value(args[1], src))};
    const auto b{TRY(resolve_value(args[2], src))};
    const auto len{pred_vec->len};
    auto&      lane{[&]() -> gs::type& {
        if (const auto va{vector_read(type_of(a))}) { return gs::vector_of(*va)->underlying; }
        if (const auto vb{vector_read(type_of(b))}) { return gs::vector_of(*vb)->underlying; }
        return comp_.ctx().default_concrete(type_of(a));
    }()};
    const auto av{TRY(vector_operand(a, lane, len, src))};
    const auto bv{TRY(vector_operand(b, lane, len, src))};
    auto&      result_type{comp_.ctx().get_vector(gs::types::mut::CONSTANT, len, lane)};

    // Known lanes are picked one at a time
    const auto known_pred{lanes_of(pred, len)};
    const auto known_a{lanes_of(av, len)};
    const auto known_b{lanes_of(bv, len)};
    if (known_pred && known_a && known_b) {
        auto&                                pool{comp_.constants()};
        std::vector<stdx::option<ip::index>> elements;
        for (u32 i{0}; i < len; ++i) {
            const auto picked{pool.value_at((*known_pred)[i]).data.as_opt<bool>()};
            elements.emplace_back(picked && *picked ? (*known_a)[i] : (*known_b)[i]);
        }

        return operand{pool.intern(ip::aggregate_value{std::move(elements)}, result_type)};
    }

    TRY(vector_at_runtime(src));
    const auto pv{TRY(as_runtime(pred, src))};
    const auto lv{TRY(as_runtime(av, src))};
    const auto rv{TRY(as_runtime(bv, src))};
    return operand{gir::value{
        builder_.emit_vector_op(gir::instruction_kind::SELECT, {pv, lv, rv}, result_type),
        result_type}};
}

} // namespace ghoti::uir::sema
