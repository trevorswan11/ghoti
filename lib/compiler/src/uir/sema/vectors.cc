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

auto analyzer::vector_read(gs::type& type) -> stdx::option<gs::type&> {
    const auto pointee{type.get_kind() == gs::type_kind::REFERENCE ? gs::pointee_of(type)
                                                                   : stdx::none};
    auto&      read{pointee ? *pointee : type};
    return gs::is_vector(read) ? stdx::option<gs::type&>{read} : stdx::none;
}

auto analyzer::lanes_of(const operand& op, u32 len) -> stdx::option<std::vector<ip::index>> {
    const auto known{built_value(op)};
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

    TRY(require_runtime(src));
    const auto value{TRY(as_runtime(lane, src))};
    return operand{
        gir::value{builder_.emit_vector_op(gir::instruction_kind::SPLAT, {value}, vector), vector}};
}

auto analyzer::vector_operand(const operand& op, gs::type& lane, u32 len, const source& src)
    -> result<operand> {
    auto&      shape{comp_.ctx().get_vector(gs::types::mut::CONSTANT, len, lane)};
    const auto read{TRY(read_through_reference(op, src))};
    if (vector_read(type_of(read)) || read.is<anon_init>()) {
        return coerce_to(read, shape, src, {.what = use_site::kind::STORE});
    }

    return splat(read, shape, src);
}

auto analyzer::runtime_lanes(const operand& given, const operand& widened, const source& src)
    -> result<gir::value> {
    const auto read{TRY(read_through_reference(given, src))};
    auto&      shape{type_of(widened)};
    if (vector_read(type_of(read)) || read.is<anon_init>() || !comptime_value(widened)) {
        return as_runtime(widened, src);
    }

    // A known scalar is splat at runtime as the old emitter does
    const auto lane{TRY(
        coerce_to(read, gs::vector_of(shape)->underlying, src, {.what = use_site::kind::STORE}))};
    const auto value{TRY(as_runtime(lane, src))};
    return gir::value{builder_.emit_vector_op(gir::instruction_kind::SPLAT, {value}, shape), shape};
}

auto analyzer::remember_built(const gir::value& built, ip::index known) -> void {
    if (const auto local{built.data.as_opt<gir::local_id>()}; local && local->is_temp()) {
        built_values_.insert_or_assign(
            local->get_index(), built_temp{.fn = gir_fn_ ? &*gir_fn_ : nullptr, .value = known});
    }
}

auto analyzer::built_value(const operand& op) const -> stdx::option<ip::index> {
    if (const auto known{comptime_value(op)}) { return known; }
    if (const auto slot{op.as_opt<slot_value>()}) {
        return slot->known ? slot->known : slot->built;
    }
    const auto value{op.as_opt<gir::value>()};
    const auto local{value ? value->data.as_opt<gir::local_id>() : stdx::none};
    if (!local || !local->is_temp()) { return stdx::none; }
    const auto found{built_values_.find(local->get_index())};
    if (found == built_values_.end() || !gir_fn_ || found->second.fn != &*gir_fn_) {
        return stdx::none;
    }

    return found->second.value;
}

auto analyzer::bind_known_vector(ip::index known, const source& src) -> result<operand> {
    const auto built{TRY(as_runtime(operand{known}, src))};
    remember_built(built, known);
    return operand{built};
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
        TRY(require_runtime(src));
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

        TRY(require_runtime(src));
        const auto value{TRY(as_runtime(op, src))};
        return stdx::option<operand>{operand{gir::value{value.data, target}}};
    }

    // An array and a vector of the same lanes convert into each other by value
    if (gs::is_array_vector_pair(from, target)) {
        const auto slot{op.as_opt<slot_value>()};
        const auto known{slot && slot->known ? slot->known : comptime_value(op)};
        if (known && (is_comptime() || !gir_fn_)) {
            const auto data{pool.value_at(*known).data};
            return stdx::option<operand>{operand{pool.intern(data, target)}};
        }

        // A known value is still repacked at runtime, as the old emitter does
        TRY(require_runtime(src));
        const auto       built{built_value(op)};
        const auto       value{TRY(as_runtime(op, src))};
        const gir::value repacked{
            builder_.emit_vector_op(gir::instruction_kind::REPACK, {value}, target), target};
        if (built) { remember_built(repacked, pool.intern(pool.value_at(*built).data, target)); }
        return stdx::option<operand>{operand{repacked}};
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

    TRY(require_runtime(src));
    const auto lv{TRY(runtime_lanes(lhs, l, src))};
    const auto rv{TRY(runtime_lanes(rhs, r, src))};
    if (is_comparison) {
        return gir::value{builder_.emit_binary(info.kind, lv, rv, result_type), result_type};
    }

    const auto op{gir::semantics::int_op_of(info.kind, info.wrapping, info.saturating)};
    const bool checked{op && checks_overflow(*op, lane)};
    return gir::value{
        builder_.emit_binary(info.kind, lv, rv, result_type, checked, info.saturating),
        result_type};
}

auto analyzer::vector_unary(tag t, const operand& value, const source& src) -> result<operand> {
    const auto read{TRY(read_through_reference(value, src))};
    auto&      type{type_of(read)};
    const auto vec{*gs::vector_of(type)};
    auto&      lane{vec.underlying};
    const bool fits{[&] {
        switch (t) {
        case tag::pos:
        case tag::neg:
        case tag::neg_wrap: return gs::is_numeric(lane.get_kind());
        case tag::bit_not:  return is_int_like(lane);
        case tag::bool_not: return is_bool(lane);
        default:            return false;
        }
    }()};
    if (!fits) { return comp_.unsupported(u_, src, "this unary operator on this vector"); }
    if (t == tag::pos) { return read; }

    // Known lanes fold one at a time, any that doesn't leaving the whole vector to runtime
    if (const auto lanes{lanes_of(read, vec.len)}) {
        std::vector<stdx::option<ip::index>> elements;
        for (const auto known : *lanes) {
            const auto folded{fold_unary(t, operand{known}, lane)};
            const auto index{folded ? comptime_value(*folded) : stdx::none};
            if (!index) { break; }
            elements.emplace_back(*index);
        }

        if (elements.size() == vec.len) {
            return operand{
                comp_.constants().intern(ip::aggregate_value{std::move(elements)}, type)};
        }
    }

    TRY(require_runtime(src));
    const auto v{TRY(as_runtime(read, src))};
    auto&      result_type{*v.type};
    using gir::instruction_kind;
    if (t == tag::bool_not) {
        return gir::value{builder_.emit_unary(instruction_kind::NOT, v, result_type), result_type};
    }

    if (t == tag::bit_not) {
        return gir::value{builder_.emit_unary(instruction_kind::BITNOT, v, result_type),
                          result_type};
    }

    // `NEG` is signed-only, so an unsigned `-%v` is the wrapping `0 -% v` it means
    if (gs::is_integer(lane.get_kind()) && !gs::is_signed_integer(lane)) {
        if (t != tag::neg_wrap) {
            return comp_.unsupported(u_, src, "negating a vector of unsigned integers");
        }

        const auto zero{
            constant(comp_.constants().intern(ip::zero_value{}, result_type), result_type)};
        return gir::value{builder_.emit_binary(instruction_kind::SUB, zero, v, result_type),
                          result_type};
    }

    const bool checked{t == tag::neg && runtime_safety() && gs::is_integer(lane.get_kind()) &&
                       gir::semantics::any(gir::semantics::runtime_checks(
                           gir::semantics::int_unary_op::NEG, {.bits = 1, .is_signed = true}))};
    return gir::value{builder_.emit_unary(instruction_kind::NEG, v, result_type, checked),
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

    TRY(require_runtime(src));
    const auto vec_value{TRY(as_runtime(read, src))};
    const auto idx{TRY(as_runtime(at, src))};
    if (runtime_safety() && !known_at) {
        auto&      bool_type{builtin(gs::type_kind::BOOL)};
        const auto in_bounds{builder_.emit_binary(
            gir::instruction_kind::LT, idx, usize_constant(vec.len), bool_type)};
        TRY(guard(gir::value{in_bounds, bool_type}, "index out of bounds", src));
    }

    // A known lane of a known vector is still read at runtime, but folds into what uses it
    auto&            lane{vec.underlying};
    const gir::value extracted{
        builder_.emit_vector_op(gir::instruction_kind::EXTRACT_LANE, {vec_value, idx}, lane), lane};
    const auto lanes{lanes_of(read, vec.len)};
    const auto i{known_at ? int_of(comp_.constants().value_at(*known_at)) : stdx::none};
    const auto small{i ? i->to_u128() : stdx::none};
    if (lanes && small && *small < vec.len) {
        remember_built(extracted, (*lanes)[static_cast<usize>(*small)]);
    }

    return operand{extracted};
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
                                       .wrapping = gs::is_integer(lane.get_kind())};
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

    TRY(require_runtime(src));
    const auto value{TRY(as_runtime(vector, src))};
    return operand{gir::value{
        builder_.emit_vector_op(gir::instruction_kind::REDUCE, {value}, lane, ordinal), lane}};
}

auto analyzer::lane_error(syntax::builtin_t which, std::string message, const source& src)
    -> stdx::err<stop> {
    return comp_.report(u_,
                        src,
                        gs::error::TYPE_MISMATCH,
                        fmt::format("'{}': {}", syntax::builtin_name(which), message));
}

auto analyzer::builtin_shuffle(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (args.size() != 3) { return comp_.unsupported(u_, src, "a `@shuffle` of this arity"); }
    auto&      ctx{comp_.ctx()};
    auto&      pool{comp_.constants()};
    const auto display{[&](const operand& op) { return ctx.type_display_name(type_of(op)); }};
    const auto fail{[&](usize arg, std::string message) {
        return lane_error(
            syntax::builtin_t::SHUFFLE, std::move(message), arg_src(src, arg).value_or(src));
    }};

    const auto a_given{TRY(resolve_value(args[0], src))};
    const auto a{TRY(read_through_reference(a_given, src))};
    const auto a_vec{gs::vector_of(type_of(a))};
    if (!a_vec) { return fail(0, fmt::format("expects a vector; found '{}'", display(a))); }
    auto& lane{a_vec->underlying};

    // `undefined` stands for a vector of the first operand's shape
    const auto b_given{TRY(resolve_value(args[1], src))};
    const bool b_undefined{type_of(b_given).get_kind() == gs::type_kind::UNDEFINED};
    const auto b{b_undefined ? operand{pool.intern(ip::undefined_value{}, type_of(a))}
                             : TRY(read_through_reference(b_given, src))};
    const auto b_vec{gs::vector_of(type_of(b))};
    if (!b_vec || !gs::is_same_unqualified(b_vec->underlying, lane)) {
        return fail(1,
                    fmt::format("expects a vector of '{}' lanes; found '{}'",
                                ctx.type_display_name(lane),
                                display(b)));
    }

    // A `.{ 0, 1, -1 }` mask is that many `i32`s, each known at compile time
    auto       mask{TRY(resolve_value(args[2], src))};
    auto&      i32_type{ctx.get_int(32, true)};
    const auto init{mask.as_opt<anon_init>()};
    if (init && mod().tag_of(init->inst) == tag::init_array) {
        auto rec{record(init->inst)};
        rec.skip_ref();
        auto& array{
            ctx.get_array(gs::types::mut::CONSTANT, false, rec.refs_field().size(), i32_type)};
        mask = TRY(coerce_to(mask, array, src, {.what = use_site::kind::STORE}));
    }

    const auto known_mask{built_value(mask)};
    const auto agg{known_mask ? pool.value_at(*known_mask).data.as_opt<ip::aggregate_value>()
                              : stdx::none};
    if (!agg || agg->elements.empty()) {
        return fail(2, "the mask must be a compile-time array of lane indices");
    }

    if (agg->elements.size() > gs::MAX_VECTOR_LEN) {
        return fail(2,
                    fmt::format("the mask selects {} lanes; a vector has at most {}",
                                agg->elements.size(),
                                gs::MAX_VECTOR_LEN));
    }

    // An entry picks a lane of `a`, or with `~i` lane `i` of `b`, and `undefined` leaves it open
    std::vector<stdx::option<i64>> entries;
    const auto                     a_len{static_cast<i64>(a_vec->len)};
    const auto                     b_len{static_cast<i64>(b_vec->len)};
    for (usize i{0}; i < agg->elements.size(); ++i) {
        const auto element{agg->elements[i]};
        const auto exact{element ? int_of(pool.value_at(*element)) : stdx::none};
        const auto small{exact ? exact->to_i128() : stdx::none};
        if (!small) {
            entries.emplace_back(stdx::none);
            continue;
        }

        const auto entry{static_cast<i64>(*small)};
        const bool from_a{entry >= 0};
        const auto picked{from_a ? entry : ~entry};
        if (picked >= (from_a ? a_len : b_len)) {
            return fail(2,
                        fmt::format("mask entry {} ({}) selects lane {} of '{}', which has {}",
                                    i,
                                    entry,
                                    picked,
                                    from_a ? display(a) : display(b),
                                    from_a ? a_len : b_len));
        }

        entries.emplace_back(entry);
    }

    auto& result_type{ctx.get_vector(
        gs::types::mut::CONSTANT, static_cast<u32>(entries.size()), ctx.as_lane(lane))};

    // Known lanes are picked at compile time
    const auto known_a{lanes_of(a, a_vec->len)};
    const auto known_b{b_undefined ? stdx::none : lanes_of(b, b_vec->len)};
    if (known_a && (known_b || b_undefined)) {
        std::vector<stdx::option<ip::index>> picked;
        for (const auto entry : entries) {
            if (!entry) {
                picked.emplace_back(stdx::none);
            } else if (*entry >= 0) {
                picked.emplace_back((*known_a)[static_cast<usize>(*entry)]);
            } else if (known_b) {
                picked.emplace_back((*known_b)[static_cast<usize>(~*entry)]);
            } else {
                picked.emplace_back(stdx::none);
            }
        }

        return operand{pool.intern(ip::aggregate_value{std::move(picked)}, result_type)};
    }

    // LLVM numbers `b`'s lanes after the longer operand's, which the shorter one is padded to
    TRY(require_runtime(src));
    auto&                   i64_type{ctx.get_int(64, true)};
    const auto              offset{std::max(a_len, b_len)};
    std::vector<gir::value> operands{TRY(as_runtime(a, src)), TRY(as_runtime(b, src))};
    for (const auto entry : entries) {
        if (!entry) {
            operands.emplace_back(constant(pool.intern(ip::undefined_value{}, i64_type), i64_type));
            continue;
        }

        const auto at{*entry >= 0 ? *entry : offset + ~*entry};
        operands.emplace_back(constant(pool.intern(at, i64_type), i64_type));
    }

    return operand{gir::value{
        builder_.emit_vector_op(gir::instruction_kind::SHUFFLE, std::move(operands), result_type),
        result_type}};
}

auto analyzer::builtin_masked(syntax::builtin_t which, gsl::span<const ref> args, const source& src)
    -> result<operand> {
    using syntax::builtin_t;
    if (args.size() != 3) { return comp_.unsupported(u_, src, "a masked builtin of this arity"); }
    TRY(require_runtime(src));
    auto&      ctx{comp_.ctx()};
    auto&      pool{comp_.constants()};
    const auto fail{[&](usize arg, std::string message) {
        return lane_error(which, std::move(message), arg_src(src, arg).value_or(src));
    }};
    const auto display{[&](const operand& op) { return ctx.type_display_name(type_of(op)); }};
    std::vector<operand> given;
    for (const auto arg : args) {
        const auto value{TRY(resolve_value(arg, src))};
        given.emplace_back(TRY(read_through_reference(value, src)));
    }

    // A pointer or a vector of pointers, and the lane type they point to
    const bool  is_load{which == builtin_t::MASKED_LOAD || which == builtin_t::GATHER};
    const bool  many{which == builtin_t::GATHER || which == builtin_t::SCATTER};
    const usize ptr_i{which == builtin_t::SCATTER ? 1UZ : 0UZ};
    auto&       ptr_type{type_of(given[ptr_i])};
    const auto  ptrs{many ? gs::vector_of(ptr_type) : stdx::none};
    auto&       slot{ptrs ? ptrs->underlying : ptr_type};
    const auto  lane{slot.get_kind() == gs::type_kind::POINTER ? gs::pointee_of(slot) : stdx::none};
    if (!lane || (many && !ptrs)) {
        return fail(ptr_i,
                    fmt::format("expects {}; found '{}'",
                                many ? "a vector of pointers" : "a pointer",
                                display(given[ptr_i])));
    }

    if (!is_load && slot.is_constant()) {
        return fail(ptr_i,
                    fmt::format("can't store through '{}', which points to constant memory",
                                ctx.type_display_name(slot)));
    }

    // Memory holds a `bool` as a byte, which a one-bit `bool` lane doesn't match
    if (lane->get_kind() == gs::type_kind::BOOL) {
        return fail(ptr_i,
                    "a 'bool' lane has no memory form; load 'u8' lanes and compare them instead");
    }

    auto&      bool_type{builtin(gs::type_kind::BOOL)};
    const auto mask_of{[&](usize i, u32 len) -> result<gir::value> {
        auto&      shape{ctx.get_vector(gs::types::mut::CONSTANT, len, bool_type)};
        const auto vec{gs::vector_of(type_of(given[i]))};
        if (!given[i].is<anon_init>() &&
            (!vec || vec->len != len || vec->underlying.get_kind() != gs::type_kind::BOOL)) {
            return fail(i,
                        fmt::format("the mask must be a '@Vector({}, bool)'; found '{}'",
                                    len,
                                    display(given[i])));
        }

        return as_runtime(TRY(coerce_to(given[i], shape, src, {.what = use_site::kind::STORE})),
                          src);
    }};
    const auto name{syntax::builtin_name(which)};

    if (is_load) {
        const auto mask_vec{gs::vector_of(type_of(given[1]))};
        const auto len{ptrs ? ptrs->len : mask_vec ? mask_vec->len : 0U};
        if (len == 0) {
            return fail(1,
                        fmt::format("the mask must be a '@Vector(N, bool)'; found '{}'",
                                    display(given[1])));
        }

        auto&      result_type{ctx.get_vector(gs::types::mut::CONSTANT, len, ctx.as_lane(*lane))};
        const auto mask{TRY(mask_of(1, len))};

        // The lanes kept where the mask is false, `undefined` keeping none
        const auto passthru{[&]() -> result<gir::value> {
            if (type_of(given[2]).get_kind() == gs::type_kind::UNDEFINED) {
                return constant(pool.intern(ip::undefined_value{}, result_type), result_type);
            }

            const auto vec{vector_read(type_of(given[2]))};
            if (!given[2].is<anon_init>() && (!vec || !gs::is_assignable(*vec, result_type))) {
                return fail(2,
                            fmt::format("the lanes kept where the mask is false must be a '{}'; "
                                        "found '{}'",
                                        ctx.type_display_name(result_type),
                                        display(given[2])));
            }

            return as_runtime(
                TRY(coerce_to(given[2], result_type, src, {.what = use_site::kind::STORE})), src);
        }};
        const auto ptr{TRY(as_runtime(given[0], src))};
        const auto kept{TRY(passthru())};
        const auto loaded{builder_.emit_builtin_call(name, {ptr, mask, kept}, result_type)};
        return operand{gir::value{*loaded, result_type}};
    }

    // A store writes a vector of the pointed-to lanes
    const usize value_i{which == builtin_t::SCATTER ? 0UZ : 1UZ};
    const auto  value_vec{gs::vector_of(type_of(given[value_i]))};
    if (!value_vec || !gs::is_same_unqualified(value_vec->underlying, *lane) ||
        (ptrs && ptrs->len != value_vec->len)) {
        return fail(value_i,
                    fmt::format("expects a vector of '{}' lanes{}; found '{}'",
                                ctx.type_display_name(*lane),
                                ptrs ? fmt::format(" ({} of them)", ptrs->len) : std::string{},
                                display(given[value_i])));
    }

    const auto first{TRY(as_runtime(given[0], src))};
    const auto second{TRY(as_runtime(given[1], src))};
    const auto mask{TRY(mask_of(2, value_vec->len))};
    DISCARD(builder_.emit_builtin_call(name, {first, second, mask}, builtin(gs::type_kind::VOID_)));
    return void_value();
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

    TRY(require_runtime(src));
    const auto pv{TRY(as_runtime(pred, src))};
    const auto lv{TRY(runtime_lanes(a, av, src))};
    const auto rv{TRY(runtime_lanes(b, bv, src))};
    return operand{gir::value{
        builder_.emit_vector_op(gir::instruction_kind::SELECT, {pv, lv, rv}, result_type),
        result_type}};
}

} // namespace ghoti::uir::sema
