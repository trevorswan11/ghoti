#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/id.hh"
#include "compiler/codegen/target.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/layout.hh"
#include "compiler/gir/semantics.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/big_int.hh"
#include "support/float128.hh"
#include "support/int128.hh"

namespace ghoti::uir::sema {

namespace {

using gir::instruction_kind;
using syntax::builtin_t;

// A cast that may leave its target to the type it is used as: `let x: u8 = @intCast(y)`
[[nodiscard]] constexpr auto infers_target(builtin_t which) noexcept -> bool {
    switch (which) {
    case builtin_t::INT_CAST:
    case builtin_t::TRUNCATE:
    case builtin_t::BIT_CAST:
    case builtin_t::INT_FROM_BOOL:
    case builtin_t::FROM_BACKING_INT:
    case builtin_t::INT_FROM_FLOAT:
    case builtin_t::FLOAT_FROM_INT:
    case builtin_t::FLOAT_CAST:
    case builtin_t::SPLAT:            return true;
    default:                          return false;
    }
}

[[nodiscard]] auto is_float_like(const gs::type& t) noexcept -> bool {
    return gs::is_float(t.get_kind()) || t.get_kind() == gs::type_kind::COMPTIME_FLOAT;
}

// A scalar whose bits `@bitCast` reinterprets directly
[[nodiscard]] auto has_fixed_bit_layout(const gs::type& t) noexcept -> bool {
    switch (t.get_kind()) {
    case gs::type_kind::INT:
    case gs::type_kind::ISIZE:
    case gs::type_kind::USIZE:
    case gs::type_kind::BOOL:
    case gs::type_kind::F16:
    case gs::type_kind::F32:
    case gs::type_kind::F64:
    case gs::type_kind::F80:
    case gs::type_kind::F128:
    case gs::type_kind::POINTER: return true;
    default:                     return false;
    }
}

} // namespace

auto analyzer::builtin_cast(inst_id inst, builtin_t which, gsl::span<const ref> args)
    -> result<operand> {
    const auto& src{src_of(inst)};
    if (infers_target(which)) {
        if (args.empty() || args.size() > 2) {
            return comp_.report(
                u_,
                src,
                gs::error::ARITY_MISMATCH,
                fmt::format("Builtin expects 1 or 2 arguments, found {}", args.size()));
        }

        // The target is the type the cast is used as, once it is
        if (args.size() == 1) { return cast_ref{inst}; }
        auto& target{*TRY(resolve_type(args[0], src))};
        return apply_cast(inst, which, target, args[1]);
    }

    const auto expect{[&](usize count) -> result<void> {
        if (args.size() == count) { return {}; }
        return comp_.report(
            u_,
            src,
            gs::error::ARITY_MISMATCH,
            fmt::format("Builtin expects {} arguments, found {}", count, args.size()));
    }};
    switch (which) {
    case builtin_t::PTR_CAST:
    case builtin_t::ALIGN_CAST:
    case builtin_t::PTR_FROM_INT: {
        TRY(expect(2));
        auto& target{*TRY(resolve_type(args[0], src))};
        return apply_cast(inst, which, target, args[1]);
    }
    case builtin_t::SLICE_FROM_PTR: TRY(expect(2)); return slice_from_ptr(inst, args[0], args[1]);
    default:                        TRY(expect(1)); return unary_cast(inst, which, args[0]);
    }
}

auto analyzer::cast_target_inferred(cast_ref cast, stdx::option<gs::type&> target)
    -> result<operand> {
    const auto& src{src_of(cast.inst)};
    if (mod().tag_of(cast.inst) != tag::builtin) {
        // A shifted untyped value with no type to take is an `i32`
        const auto t{mod().tag_of(cast.inst)};
        if (!target &&
            (t == tag::shl || t == tag::shl_wrap || t == tag::shl_sat || t == tag::shr)) {
            return binary_as(cast.inst, comp_.ctx().get_int(32, true), src);
        }

        if (!target) {
            return comp_.report(
                u_,
                src,
                gs::error::TYPE_MISMATCH,
                "cannot infer the type of an inferred cast meeting an untyped value");
        }

        if (t == tag::neg || t == tag::neg_wrap || t == tag::bit_not || t == tag::pos) {
            return unary_as(cast.inst, *target, src);
        }

        return binary_as(cast.inst, *target, src);
    }

    auto       rec{record(cast.inst)};
    const auto which{static_cast<builtin_t>(rec.u32_field())};
    const auto operand_ref{rec.refs_field().front()};

    // Only `@intFromBool` has a target of its own to fall back on (u1)
    if (!target && which == builtin_t::INT_FROM_BOOL) {
        auto&      bit{comp_.ctx().get_int(1, false)};
        const auto from{TRY(resolve_value(operand_ref, src))};
        if (const auto vec{gs::vector_of(type_of(from))}) {
            target.emplace(comp_.ctx().get_vector(gs::types::mut::CONSTANT, vec->len, bit));
        } else {
            target.emplace(bit);
        }
    }

    if (!target) {
        const auto name{syntax::builtin_name(which)};
        return comp_.report(
            u_,
            src,
            gs::error::TYPE_MISMATCH,
            fmt::format("cannot infer the target type of '{}' here; write '{}(T, x)'", name, name));
    }

    if (which == builtin_t::AS) { return as_to(*target, operand_ref, src); }
    return apply_cast(cast.inst, which, *target, operand_ref);
}

auto analyzer::arg_src(const source& call_src, usize index) const -> stdx::option<source> {
    const auto node{call_src.as_opt<ast::node_id>()};
    const auto expr{node ? u_.source->ast.get_as_opt<ast::call_expr>(*node) : stdx::none};
    if (!expr || index >= expr->arguments.size()) { return stdx::none; }
    const auto arg{expr->arguments[index].as_opt<ast::expr_handle>()};
    return arg ? stdx::option<source>{source{*arg}} : stdx::none;
}

auto analyzer::cast_operand_src(inst_id call, ref operand_ref) const -> source {
    // A name's use is located by the call's argument, since its ref is the binding it names
    const auto& src{src_of(call)};
    auto        rec{record(call)};
    rec.skip_u32();
    const auto args{rec.refs_field()};
    if (const auto at{std::ranges::find(args, operand_ref)}; at != args.end()) {
        if (const auto arg{arg_src(src, static_cast<usize>(at - args.begin()))}) { return *arg; }
    }

    if (const auto i{as_inst(operand_ref)}) { return src_of(*i); }
    return src;
}

auto analyzer::folds_whole(builtin_t which, const gs::type& from, const gs::type& target) noexcept
    -> bool {
    switch (which) {
    case builtin_t::INT_FROM_FLOAT:
    case builtin_t::FLOAT_FROM_INT:
    case builtin_t::FLOAT_CAST:
    case builtin_t::FROM_BACKING_INT: return true;

    // An untyped operand has no width to reinterpret, so it folds into any scalar
    case builtin_t::BIT_CAST:
        return gs::is_comptime_numeric(from.get_kind()) && has_fixed_bit_layout(target);
    default: return false;
    }
}

auto analyzer::apply_cast(inst_id inst, builtin_t which, gs::type& target, ref operand_ref)
    -> result<operand> {
    const auto& src{src_of(inst)};
    const auto  name{syntax::builtin_name(which)};
    const auto  value{TRY(cast_operand(operand_ref, src))};
    if (value.is<cast_ref>()) {
        return comp_.unsupported(u_, src, "a cast whose operand's target is inferred");
    }

    auto& from{type_of(value)};
    if (from.is_poison() || target.is_poison()) { return stdx::err{stop::POISON}; }
    TRY(reject_runtime_to_comptime(value, target, src));

    if (which != builtin_t::SPLAT && which != builtin_t::BIT_CAST &&
        (gs::is_vector(from) || gs::is_vector(target))) {
        return vector_cast(which, value, target, src, cast_operand_src(inst, operand_ref));
    }

    // `@splat` puts its operand in every lane of the vector it is used as
    if (which == builtin_t::SPLAT) {
        if (!gs::is_vector(target)) {
            return comp_.report(u_,
                                src,
                                gs::error::TYPE_MISMATCH,
                                fmt::format("'@splat' needs a vector result; found '{}'",
                                            comp_.ctx().type_display_name(target)));
        }

        return splat(value, target, src);
    }

    const auto operand_src{cast_operand_src(inst, operand_ref)};
    const auto mismatch{[&](std::string message) -> stdx::err<stop> {
        return comp_.report(u_, src, gs::error::TYPE_MISMATCH, std::move(message));
    }};
    const auto display{[&](const gs::type& t) { return comp_.ctx().type_display_name(t); }};
    if (from.get_kind() == gs::type_kind::UNDEFINED) {
        return mismatch(fmt::format("'{}' has no value to convert in 'undefined'; assign "
                                    "'undefined' directly instead",
                                    name));
    }

    // The operand's and target's kinds each cast accepts
    switch (which) {
    case builtin_t::INT_CAST:
        if (!is_int_like(target)) {
            return mismatch(fmt::format("`@intCast` target must be an integer type; found '{}'",
                                        display(target)));
        }
        if (!is_int_like(from)) {
            return mismatch(fmt::format("`@intCast` operand must be an integer type; found '{}'",
                                        display(from)));
        }
        break;
    case builtin_t::TRUNCATE:
        if (target.get_kind() == gs::type_kind::COMPTIME_INT) {
            return mismatch("`@truncate` target 'comptime_int' has no width to truncate to; use "
                            "`@intCast`");
        }
        if (!gs::is_integer(target.get_kind())) {
            return mismatch(fmt::format("`@truncate` target must be an integer type; found '{}'",
                                        display(target)));
        }
        if (!gs::is_integer(from.get_kind())) {
            return mismatch(fmt::format("`@truncate` operand must be an integer type; found '{}'",
                                        display(from)));
        }
        if (domain_of(target).bits > domain_of(from).bits) {
            return mismatch(
                fmt::format("`@truncate` target type '{}' is wider than '{}'; use `@as` instead",
                            display(target),
                            display(from)));
        }
        break;
    case builtin_t::INT_FROM_BOOL:
        if (!gs::is_integer(target.get_kind())) {
            return mismatch(fmt::format("`@intFromBool` target must be an integer type; found '{}'",
                                        display(target)));
        }
        if (from.get_kind() != gs::type_kind::BOOL) {
            return mismatch(fmt::format("`@intFromBool` operand must be of type 'bool'; found '{}'",
                                        display(from)));
        }
        break;
    case builtin_t::INT_FROM_FLOAT:
        if (!is_int_like(target)) {
            return mismatch(fmt::format(
                "`@intFromFloat` target must be an integer type; found '{}'", display(target)));
        }
        if (!is_float_like(from)) {
            return mismatch(
                fmt::format("`@intFromFloat` operand must be a float; found '{}'", display(from)));
        }
        break;
    case builtin_t::FLOAT_FROM_INT:
        if (!is_float_like(target)) {
            return mismatch(fmt::format("`@floatFromInt` target must be a float type; found '{}'",
                                        display(target)));
        }
        if (!is_int_like(from)) {
            return mismatch(fmt::format("`@floatFromInt` operand must be an integer; found '{}'",
                                        display(from)));
        }
        break;
    case builtin_t::FLOAT_CAST:
        if (!is_float_like(target)) {
            return mismatch(fmt::format("`@floatCast` target must be a float type; found '{}'",
                                        display(target)));
        }
        if (gs::is_integer(from.get_kind())) {
            return mismatch(fmt::format("`@floatCast` operand must be a float; found '{}'; use "
                                        "`@floatFromInt` instead",
                                        display(from)));
        }
        if (!is_float_like(from) && from.get_kind() != gs::type_kind::COMPTIME_INT) {
            return mismatch(
                fmt::format("`@floatCast` operand must be a float; found '{}'", display(from)));
        }
        break;
    case builtin_t::BIT_CAST:         TRY(check_bit_cast(from, target, src)); break;
    case builtin_t::FROM_BACKING_INT: {
        const auto backing{comp_.ctx().backing_int_type(target, comp_.ptr_bits())};
        const auto un{target.get_data().as_opt<gs::types::union_t>()};
        if (!backing || (un && !un->is_bit_packed())) {
            return mismatch(fmt::format("`@fromBackingInt` target must be an enum or a packed "
                                        "struct or union; found '{}'",
                                        display(target)));
        }

        if (from.get_kind() != gs::type_kind::COMPTIME_INT &&
            !(gs::is_integer(from.get_kind()) && gs::is_assignable(from, *backing))) {
            return mismatch(fmt::format("`@fromBackingInt` operand must be an integer "
                                        "assignable to '{}', the backing integer of '{}'; "
                                        "found '{}'",
                                        display(*backing),
                                        display(target),
                                        display(from)));
        }
        break;
    }
    case builtin_t::PTR_CAST:
    case builtin_t::ALIGN_CAST:
        if (target.get_kind() != gs::type_kind::POINTER) {
            return mismatch(fmt::format(
                "'{}' target type must be a pointer; found '{}'", name, display(target)));
        }
        if (from.get_kind() != gs::type_kind::POINTER) {
            return mismatch(fmt::format("'{}' operand must be a pointer; found '{}'{}",
                                        name,
                                        display(from),
                                        from.get_kind() == gs::type_kind::REFERENCE
                                            ? " (did you mean `^` instead of `&`?)"
                                            : ""));
        }
        break;
    case builtin_t::PTR_FROM_INT:
        if (target.get_kind() != gs::type_kind::POINTER) {
            return mismatch(fmt::format("Expected a pointer type; found '{}'", display(target)));
        }
        break;
    default: return comp_.unsupported(u_, src, fmt::format("the `{}` builtin", name));
    }

    // A known operand folds at compile time, while a runtime body folds only the conversions the
    // old emitter evaluated whole, keeping the rest as casts of a constant
    const auto known{comptime_value(value)};
    if (known && (is_comptime() || folds_whole(which, from, target))) {
        return fold_cast(which, *known, from, target, src, operand_src);
    }

    TRY(require_runtime(src));
    if (!known && folds_whole(which, from, target)) {
        if (const auto folded{TRY(speculated_value(operand_ref, src))}) {
            return fold_cast(which, *folded, type_of(operand{*folded}), target, src, operand_src);
        }
    }

    // A known untyped operand is cast as itself, rather than through its default type
    auto runtime{known && gs::is_comptime_numeric(from.get_kind()) ? constant(*known, from)
                                                                   : TRY(as_runtime(value, src))};
    switch (which) {
    case builtin_t::INT_CAST: {
        // An operand known at compile time is checked there instead of guarded
        const auto checked{known ? known : TRY(speculated_value(operand_ref, src))};
        if (checked) {
            DISCARD(TRY(fold_cast(which, *checked, from, target, src, operand_src)));
        } else {
            TRY(int_cast_guard(runtime, target, operand_src));
        }

        return cast_value(instruction_kind::INT_CAST, runtime, target);
    }
    case builtin_t::TRUNCATE:      return cast_value(instruction_kind::TRUNC_CAST, runtime, target);
    case builtin_t::INT_FROM_BOOL: return cast_value(instruction_kind::INT_CAST, runtime, target);
    case builtin_t::BIT_CAST:      {
        // An untyped integer takes every bit of the target, signed when it is negative
        if (known && from.get_kind() == gs::type_kind::COMPTIME_INT) {
            const auto layout{codegen::type_layout::resolve(comp_.ctx().target_opts.triple_str)};
            const auto bits{gir::size_of(target, layout) * 8};
            const auto exact{*int_of(comp_.constants().value_at(*known))};
            auto&      width{comp_.ctx().get_int(static_cast<u16>(bits), exact.is_negative())};
            const auto widened{TRY(coerce_to(value, width, src, {.what = use_site::kind::STORE}))};
            runtime = TRY(as_runtime(widened, src));
        }

        const auto cast{cast_value(instruction_kind::BIT_CAST, runtime, target)};
        TRY(enum_cast_guard(cast, runtime, operand_ref, src));
        return cast;
    }
    case builtin_t::PTR_CAST:
    case builtin_t::ALIGN_CAST: return cast_value(instruction_kind::PTR_CAST, runtime, target);
    case builtin_t::PTR_FROM_INT:
        return cast_value(instruction_kind::PTR_FROM_INT, runtime, target);
    case builtin_t::INT_FROM_FLOAT:
        return gir::value{
            builder_.emit_cast(instruction_kind::WIDEN_CAST, runtime, target, runtime_safety()),
            target};
    case builtin_t::FLOAT_FROM_INT:
    case builtin_t::FLOAT_CAST: return cast_value(instruction_kind::WIDEN_CAST, runtime, target);
    case builtin_t::FROM_BACKING_INT: {
        auto&      backing{*comp_.ctx().backing_int_type(target, comp_.ptr_bits())};
        const auto coerced{TRY(coerce_to(value, backing, src, {.what = use_site::kind::STORE}))};
        const auto widened{TRY(as_runtime(coerced, src))};
        const auto cast{cast_value(instruction_kind::BIT_CAST, widened, target)};
        TRY(enum_cast_guard(cast, widened, operand_ref, src));
        return cast;
    }
    default: break;
    }

    return comp_.unsupported(u_, src, fmt::format("the `{}` builtin", name));
}

auto analyzer::vector_cast(builtin_t      which,
                           const operand& value,
                           gs::type&      target,
                           const source&  src,
                           const source&  operand_src) -> result<operand> {
    auto&      ctx{comp_.ctx()};
    auto&      pool{comp_.constants()};
    auto&      from{type_of(value)};
    const auto name{syntax::builtin_name(which)};
    const auto display{[&](const gs::type& t) { return ctx.type_display_name(t); }};
    const auto to_vec{gs::vector_of(target)};
    const auto from_vec{gs::vector_of(from)};
    if (!to_vec || !from_vec || to_vec->len != from_vec->len) {
        return comp_.report(u_,
                            operand_src,
                            gs::error::TYPE_MISMATCH,
                            fmt::format("'{}' converts a vector to a vector of as many lanes; "
                                        "found '{}' to '{}'",
                                        name,
                                        display(from),
                                        display(target)));
    }

    auto&      to{to_vec->underlying};
    auto&      lane{from_vec->underlying};
    const bool to_int{gs::is_integer(to.get_kind())};
    const bool to_float{gs::is_float(to.get_kind())};
    const bool from_int{gs::is_integer(lane.get_kind())};
    const bool from_float{gs::is_float(lane.get_kind())};
    const auto fits{[&] -> stdx::option<bool> {
        switch (which) {
        case builtin_t::INT_CAST: return to_int && from_int;
        case builtin_t::TRUNCATE:
            return to_int && from_int && domain_of(to).bits <= domain_of(lane).bits;
        case builtin_t::INT_FROM_BOOL:  return to_int && lane.get_kind() == gs::type_kind::BOOL;
        case builtin_t::INT_FROM_FLOAT: return to_int && from_float;
        case builtin_t::FLOAT_FROM_INT: return to_float && from_int;
        case builtin_t::FLOAT_CAST:     return to_float && from_float;
        default:                        return stdx::none;
        }
    }()};
    if (!fits) { return comp_.unsupported(u_, src, fmt::format("a `{}` of a vector", name)); }
    if (!*fits) {
        return comp_.report(
            u_,
            operand_src,
            gs::error::TYPE_MISMATCH,
            fmt::format(
                "'{}' can't convert '{}' lanes to '{}' lanes", name, display(lane), display(to)));
    }

    // Lanes fold one at a time, a runtime body folding only what the old emitter evaluated whole
    const auto known{comptime_value(value)};
    const auto built{built_value(value)};
    const auto fold_lanes{[&](ip::index whole) -> result<operand> {
        const auto agg{pool.value_at(whole).data.as_opt<ip::aggregate_value>()};
        if (!agg) { return comp_.unsupported(u_, src, fmt::format("a `{}` of this vector", name)); }
        const std::vector<stdx::option<ip::index>> lanes{agg->elements};
        std::vector<stdx::option<ip::index>>       out;
        for (const auto element : lanes) {
            if (!element) { return comp_.unsupported(u_, src, "a cast of an undefined lane"); }
            const auto folded{TRY(fold_cast(which, *element, lane, to, src, operand_src))};
            const auto index{comptime_value(folded)};
            if (!index) { return comp_.unsupported(u_, src, "a cast of a lane that doesn't fold"); }
            out.emplace_back(*index);
        }

        return operand{pool.intern(ip::aggregate_value{std::move(out)}, target)};
    }};
    if (known && is_comptime()) { return fold_lanes(*known); }
    if (built && folds_whole(which, lane, to)) { return fold_lanes(*built); }

    TRY(require_runtime(src));
    const auto runtime{TRY(as_runtime(value, src))};
    switch (which) {
    case builtin_t::INT_CAST:
        // A known vector is checked lane by lane at compile time instead of guarded
        if (built) {
            DISCARD(TRY(fold_lanes(*built)));
        } else {
            TRY(int_cast_guard(runtime, target, operand_src));
        }

        return cast_value(instruction_kind::INT_CAST, runtime, target);
    case builtin_t::TRUNCATE:      return cast_value(instruction_kind::TRUNC_CAST, runtime, target);
    case builtin_t::INT_FROM_BOOL: return cast_value(instruction_kind::INT_CAST, runtime, target);
    case builtin_t::INT_FROM_FLOAT:
        return gir::value{
            builder_.emit_cast(instruction_kind::WIDEN_CAST, runtime, target, runtime_safety()),
            target};
    default: return cast_value(instruction_kind::WIDEN_CAST, runtime, target);
    }
}

auto analyzer::cast_operand(ref operand_ref, const source& src) -> result<operand> {
    const auto value{TRY(resolve_value(operand_ref, src))};
    const auto referenced{type_of(value).get_data().as_opt<gs::types::reference>()};
    if (!referenced || comptime_value(value)) { return value; }

    // A cast reads what a reference operand refers to
    TRY(require_runtime(src));
    auto&      referent{referenced->underlying};
    const auto address{TRY(as_runtime(value, src))};
    return gir::value{builder_.emit_load(address, referent), referent};
}

auto analyzer::cast_value(instruction_kind kind, const gir::value& from, gs::type& target)
    -> operand {
    return gir::value{builder_.emit_cast(kind, from, target), target};
}

auto analyzer::check_bit_cast(const gs::type& from, const gs::type& target, const source& src)
    -> result<void> {
    const auto display{[&](const gs::type& t) { return comp_.ctx().type_display_name(t); }};
    if (gs::is_comptime_numeric(target.get_kind())) {
        return comp_.report(
            u_,
            src,
            gs::error::TYPE_MISMATCH,
            fmt::format("`@bitCast` target '{}' has no bit layout; use `@as`", display(target)));
    }

    // An untyped float has the bit pattern of an `f64`, an untyped integer takes the target's
    const auto& source_type{
        from.get_kind() == gs::type_kind::COMPTIME_FLOAT ? builtin(gs::type_kind::F64) : from};
    if (from.get_kind() == gs::type_kind::COMPTIME_INT) { return {}; }

    // An aggregate only has a bit pattern to reinterpret when it is bit-packed
    const auto is_aggregate{[](const gs::type& t) {
        return t.get_kind() == gs::type_kind::STRUCT || t.get_kind() == gs::type_kind::UNION;
    }};
    if (!gs::is_vector(source_type) && !gs::is_vector(target) &&
        (is_aggregate(source_type) || is_aggregate(target))) {
        const auto from_bits{bit_width(source_type)};
        const auto to_bits{bit_width(target)};
        if (!from_bits || !to_bits) {
            return comp_.report(u_,
                                src,
                                gs::error::TYPE_MISMATCH,
                                fmt::format("`@bitCast` needs types with a defined bit layout; "
                                            "'{}' is not a packed struct or union",
                                            display(from_bits ? target : source_type)));
        }

        if (*from_bits != *to_bits) {
            return comp_.report(u_,
                                src,
                                gs::error::TYPE_MISMATCH,
                                fmt::format("`@bitCast` requires equally sized types; '{}' is {} "
                                            "bit(s) but '{}' is {} bit(s)",
                                            display(source_type),
                                            *from_bits,
                                            display(target),
                                            *to_bits));
        }

        return {};
    }

    const auto from_bits{bit_width(source_type)};
    const auto to_bits{bit_width(target)};
    if (!from_bits || !to_bits) {
        return comp_.unsupported(u_, src, "a `@bitCast` of a struct, vector or enum");
    }

    // Widths are compared in bits, since an `f80` and a `u80` may be padded apart
    if (*from_bits != *to_bits) {
        const bool bytes{*from_bits % 8 == 0 && *to_bits % 8 == 0};
        const auto unit{bytes ? "byte(s)" : "bit(s)"};
        return comp_.report(
            u_,
            src,
            gs::error::TYPE_MISMATCH,
            fmt::format("`@bitCast` requires equally sized types; '{}' is {} {} but '{}' is {} {}",
                        display(source_type),
                        bytes ? *from_bits / 8 : *from_bits,
                        unit,
                        display(target),
                        bytes ? *to_bits / 8 : *to_bits,
                        unit));
    }

    return {};
}

auto analyzer::bit_width(const gs::type& t) const -> stdx::option<u64> {
    if (const auto arr{t.get_data().as_opt<gs::types::array>()}) {
        // Elements lie end to end only when each fills its storage exactly
        const auto elem{bit_width(arr->underlying)};
        const auto layout{codegen::type_layout::resolve(comp_.ctx().target_opts.triple_str)};
        if (!elem || arr->null_terminated || *elem != gir::size_of(arr->underlying, layout) * 8) {
            return stdx::none;
        }

        return *elem * arr->len;
    }

    // A bit-packed aggregate is its backing integer
    const auto ptr_bits{comp_.ptr_bits()};
    if (const auto st{t.get_data().as_opt<gs::types::struct_t>()}) {
        if (!st->is_bit_packed()) { return stdx::none; }
        return gs::packed_backing_bits(*st, ptr_bits).transform([](u32 b) { return u64{b}; });
    }

    if (const auto ut{t.get_data().as_opt<gs::types::union_t>()}) {
        if (!ut->is_bit_packed()) { return stdx::none; }
        return gs::packed_union_backing_bits(*ut, ptr_bits).transform([](u32 b) { return u64{b}; });
    }

    // A vector's lanes lie end to end, a `bool` lane taking one bit
    if (const auto vec{gs::vector_of(t)}) {
        const auto lane{vec->underlying.get_kind() == gs::type_kind::BOOL
                            ? stdx::option<u64>{1}
                            : bit_width(vec->underlying)};
        return lane.transform([&](u64 bits) { return bits * vec->len; });
    }

    if (!has_fixed_bit_layout(t)) { return stdx::none; }
    if (const auto format{gs::float_format_of(t)}) { return format_info(*format).storage_bits; }
    if (t.get_kind() == gs::type_kind::BOOL) { return u64{8}; }
    if (t.get_kind() == gs::type_kind::POINTER) { return u64{comp_.ptr_bits()}; }
    return u64{domain_of(t).bits};
}

auto analyzer::bits_of(ip::index value, const gs::type& type, const source& src) -> result<u128> {
    auto&      pool{comp_.constants()};
    const auto data{pool.value_at(value).data};

    // A bit-packed aggregate's bits are its fields' at their offsets, a union's at offset zero
    const auto ptr_bits{comp_.ptr_bits()};
    const auto field_bits{[&](ip::index field, const gs::type& field_type) -> result<u128> {
        const auto bits{TRY(bits_of(field, field_type, src))};
        const auto width{gs::packed_field_bits(field_type, ptr_bits).value_or(128)};
        return width >= 128 ? bits : bits & ((u128{1} << width) - 1);
    }};
    if (const auto st{type.get_data().as_opt<gs::types::struct_t>()}; st && st->is_bit_packed()) {
        const auto agg{data.as_opt<ip::aggregate_value>()};
        if (!agg) { return comp_.unsupported(u_, src, "the bits of this packed value"); }
        u128 out{0};
        for (usize i{0}; i < agg->elements.size(); ++i) {
            if (!agg->elements[i]) { continue; }
            out |= TRY(field_bits(*agg->elements[i], st->type_at(i)))
                   << gs::packed_field_offset(*st, i, ptr_bits);
        }
        return out;
    }

    if (const auto ut{type.get_data().as_opt<gs::types::union_t>()}; ut && ut->is_bit_packed()) {
        const auto un{data.as_opt<ip::union_value>()};
        if (!un || un->payload.empty()) {
            return comp_.unsupported(u_, src, "the bits of this packed value");
        }

        const auto field{std::ranges::find(ut->field_names, un->active_field)};
        const auto index{static_cast<usize>(field - ut->field_names.begin())};
        return field_bits(un->payload.front(), ut->type_at(index));
    }

    if (const auto vec{gs::vector_of(type)}) {
        const auto& lane{vec->underlying};
        const auto  width{lane.get_kind() == gs::type_kind::BOOL ? u64{1} : *bit_width(lane)};
        const auto  agg{data.as_opt<ip::aggregate_value>()};
        u128        out{0};
        for (usize i{0}; i < vec->len; ++i) {
            if (!agg || i >= agg->elements.size() || !agg->elements[i]) {
                return comp_.unsupported(u_, src, "a `@bitCast` of an undefined lane");
            }

            out |= TRY(bits_of(*agg->elements[i], lane, src)) << (i * width);
        }
        return out;
    }

    if (const auto arr{type.get_data().as_opt<gs::types::array>()}) {
        const auto width{*bit_width(arr->underlying)};
        const auto agg{data.as_opt<ip::aggregate_value>()};
        const auto text{data.as_opt<std::string>()};
        u128       out{0};
        for (usize i{0}; i < arr->len; ++i) {
            u128 element{0};
            if (agg && i < agg->elements.size() && agg->elements[i]) {
                element = TRY(bits_of(*agg->elements[i], arr->underlying, src));
            } else if (text && i < text->size()) {
                element = static_cast<u8>((*text)[i]);
            } else {
                return comp_.unsupported(u_, src, "a `@bitCast` of an undefined element");
            }
            out |= element << (i * width);
        }
        return out;
    }

    if (const auto b{data.as_opt<bool>()}) { return u128{*b ? 1U : 0U}; }
    if (const auto number{data.as_opt<f128>()}) {
        return number->encode(*gs::float_format_of(type));
    }

    if (const auto exact{int_of(pool.value_at(value))}) {
        const auto domain{domain_of(type)};
        const auto low{exact->to_i128() ? static_cast<u128>(*exact->to_i128()) : *exact->to_u128()};
        return domain.bits >= 128 ? low : low & ((u128{1} << domain.bits) - 1);
    }

    return comp_.unsupported(u_, src, "a `@bitCast` of this value at compile time");
}

auto analyzer::value_of_bits(u128 bits, gs::type& type, const source& src) -> result<ip::index> {
    auto& pool{comp_.constants()};
    if (const auto vec{gs::vector_of(type)}) {
        auto&      lane{vec->underlying};
        const auto width{lane.get_kind() == gs::type_kind::BOOL ? u64{1} : *bit_width(lane)};
        const auto mask{width >= 128 ? ~u128{0} : (u128{1} << width) - 1};
        std::vector<stdx::option<ip::index>> lanes;
        for (usize i{0}; i < vec->len; ++i) {
            lanes.emplace_back(TRY(value_of_bits((bits >> (i * width)) & mask, lane, src)));
        }
        return pool.intern(ip::aggregate_value{std::move(lanes)}, type);
    }

    if (const auto arr{type.get_data().as_opt<gs::types::array>()}) {
        const auto                           width{*bit_width(arr->underlying)};
        const auto                           mask{width >= 128 ? ~u128{0} : (u128{1} << width) - 1};
        std::vector<stdx::option<ip::index>> elements;
        for (usize i{0}; i < arr->len; ++i) {
            elements.emplace_back(
                TRY(value_of_bits((bits >> (i * width)) & mask, arr->underlying, src)));
        }
        return pool.intern(ip::aggregate_value{std::move(elements)}, type);
    }

    if (type.get_kind() == gs::type_kind::BOOL) { return pool.intern(bits != 0, type); }
    if (const auto format{gs::float_format_of(type)}) {
        return gir::value::constant(pool, f128::decode(bits, *format), type).constant_index();
    }

    if (gs::is_integer(type.get_kind()) && folds_exactly(type)) {
        const auto domain{domain_of(type)};
        const auto kept{gir::semantics::truncate_int(domain, bits)};
        return intern_int(pool,
                          domain.is_signed ? big_int{gir::semantics::signed_value(kept, domain)}
                                           : big_int{gir::semantics::unsigned_value(kept, domain)},
                          type);
    }

    return comp_.unsupported(u_, src, "a `@bitCast` to this type at compile time");
}

auto analyzer::fold_cast(builtin_t       which,
                         ip::index       index,
                         const gs::type& from,
                         gs::type&       target,
                         const source&   src,
                         const source&   operand_src) -> result<operand> {
    auto&       pool{comp_.constants()};
    auto&       ctx{comp_.ctx()};
    const auto& v{pool.value_at(index)};
    const auto  exact{int_of(v)};
    const auto  number{v.data.as_opt<f128>()};
    const auto  display{[&](const gs::type& t) { return ctx.type_display_name(t); }};
    const auto  fits{[&](const big_int& value) {
        return target.get_kind() == gs::type_kind::COMPTIME_INT ||
               gs::comptime_int_fits(value, target, comp_.ptr_bits());
    }};

    // The low `bits` of an integer of any width
    const auto low_bits{[](const big_int& value) -> stdx::option<u128> {
        if (const auto small{value.to_i128()}) { return static_cast<u128>(*small); }
        return value.to_u128();
    }};
    const auto format_of{[&](const gs::type& t) {
        return t.get_kind() == gs::type_kind::COMPTIME_FLOAT ? float_format::QUAD
                                                             : *gs::float_format_of(t);
    }};

    switch (which) {
    case builtin_t::INT_CAST:
        if (!fits(*exact)) {
            return comp_.report(
                u_,
                operand_src,
                gs::error::COMPTIME_EVALUATION_FAILED,
                fmt::format("Integer value {} is out of range for target type '{}' in @intCast",
                            exact->to_string(),
                            display(target)));
        }
        return intern_int(pool, *exact, target);
    case builtin_t::TRUNCATE: {
        const auto domain{domain_of(target)};
        return intern_int(pool, wrap_int(*exact, domain.bits, domain.is_signed), target);
    }
    case builtin_t::INT_FROM_BOOL: {
        const bool b{*v.data.as_opt<bool>()};
        return intern_int(pool, big_int{static_cast<u128>(b ? 1 : 0)}, target);
    }
    case builtin_t::FROM_BACKING_INT: {
        const auto n{comp_.nominal_of(target)};
        if (!n || !exact) { return comp_.unsupported(u_, src, "a `@fromBackingInt` of this type"); }
        const auto  small{exact->to_i128()};
        const bool  variant{small &&
                            std::ranges::contains(n->enum_values, static_cast<i64>(*small))};
        const auto& en{target.get_data().as<gs::types::enum_t>()};
        if (!variant && !en.non_exhaustive) {
            return comp_.report(
                u_,
                src,
                gs::error::COMPTIME_EVALUATION_FAILED,
                fmt::format("{} is not a value of enum '{}'", exact->to_string(), display(target)));
        }

        return pool.intern(static_cast<i64>(*small), target);
    }
    case builtin_t::BIT_CAST: {
        // An array or vector reinterprets its elements' bits end to end
        const auto lined{[](const gs::type& t) {
            return t.get_kind() == gs::type_kind::ARRAY || t.get_kind() == gs::type_kind::VECTOR;
        }};

        // The old evaluator leaves a vector's bits to runtime, which its mirrored folds keep
        if (comp_.speculating() && (gs::is_vector(from) || gs::is_vector(target))) {
            return comp_.needs_runtime(u_, src);
        }

        if (lined(from) || lined(target)) {
            const auto facts{codegen::target_facts::resolve(ctx.target_opts.triple_str)};
            const auto width{bit_width(target)};
            if (facts.endian != "little" || !width || *width > 128) {
                return comp_.unsupported(u_, src, "this `@bitCast` of an array at compile time");
            }

            const auto bits{TRY(bits_of(index, from, src))};
            return operand{TRY(value_of_bits(bits, target, src))};
        }

        // An untyped integer takes the target's width, an untyped float an `f64`'s pattern
        const gs::type& source_type{
            from.get_kind() == gs::type_kind::COMPTIME_FLOAT ? builtin(gs::type_kind::F64) : from};
        stdx::option<u128> bits;
        if (exact) {
            bits = low_bits(*exact);
        } else if (number) {
            bits = number->encode(format_of(source_type));
        }

        // An integer keeps its pattern at any width
        if (exact && gs::is_integer(target.get_kind())) {
            const auto domain{domain_of(target)};
            return intern_int(pool, wrap_int(*exact, domain.bits, domain.is_signed), target);
        }

        if (!bits || (gs::is_integer(target.get_kind()) && !folds_exactly(target))) {
            return comp_.unsupported(u_, src, "a `@bitCast` of this value at compile time");
        }

        if (gs::is_float(target.get_kind())) {
            return gir::value::constant(pool, f128::decode(*bits, format_of(target)), target)
                .constant_index();
        }

        if (gs::is_integer(target.get_kind())) {
            const auto domain{domain_of(target)};
            const auto kept{gir::semantics::truncate_int(domain, *bits)};
            return intern_int(pool,
                              domain.is_signed
                                  ? big_int{gir::semantics::signed_value(kept, domain)}
                                  : big_int{gir::semantics::unsigned_value(kept, domain)},
                              target);
        }

        return comp_.unsupported(u_, src, "a `@bitCast` of this value at compile time");
    }
    case builtin_t::INT_FROM_FLOAT: {
        if (!number) { return comp_.unsupported(u_, src, "an `@intFromFloat` of this value"); }
        const auto truncated{number->trunc()};
        const auto whole{truncated.is_negative()
                             ? truncated.to_int().transform([](i128 v) { return big_int{v}; })
                             : truncated.to_uint().transform([](u128 v) { return big_int{v}; })};
        if (!whole || number->is_nan() || !fits(*whole)) {
            return comp_.report(u_,
                                src,
                                gs::error::COMPTIME_EVALUATION_FAILED,
                                fmt::format("Float value {} is out of range for target type '{}' "
                                            "in @intFromFloat",
                                            number->to_string(),
                                            display(target)));
        }

        return intern_int(pool, *whole, target);
    }
    case builtin_t::FLOAT_FROM_INT: {
        if (!exact) { return comp_.unsupported(u_, src, "a `@floatFromInt` of this value"); }
        const auto as_float{float_of_int(*exact, format_of(target))};
        return gir::value::constant(pool, as_float, target).constant_index();
    }
    case builtin_t::FLOAT_CAST: {
        if (exact) {
            return gir::value::constant(pool, float_of_int(*exact, format_of(target)), target)
                .constant_index();
        }

        return gir::value::constant(pool, number->round_to(format_of(target)), target)
            .constant_index();
    }
    case builtin_t::PTR_FROM_INT: {
        // A pointer known at compile time is its address as an integer, a negative one wrapping
        const auto address{exact ? wrap_int(*exact, comp_.ptr_bits(), false).to_u128()
                                 : stdx::none};
        if (!address || *address > std::numeric_limits<u64>::max()) {
            return comp_.unsupported(u_, src, "a `@ptrFromInt` of this value at compile time");
        }

        return pool.intern(static_cast<u64>(*address), target);
    }
    case builtin_t::PTR_CAST: {
        // A known address keeps what it points at under its new type
        if (const auto address{v.data.as_opt<ip::address_value>()}) {
            return pool.intern(ip::address_value{*address}, target);
        }

        if (v.data.is<ip::nullptr_value>()) { return pool.intern(ip::nullptr_value{}, target); }
        break;
    }
    default: break;
    }

    // Speculating, the cast only means the value is computed at runtime
    if (comp_.speculating()) { return comp_.needs_runtime(u_, src); }
    return comp_.unsupported(u_, src, "a pointer cast at compile time");
}

auto analyzer::unary_cast(inst_id inst, builtin_t which, ref operand_ref) -> result<operand> {
    const auto& src{src_of(inst)};
    const auto  value{TRY(which == builtin_t::BOOL_FROM_INT ? cast_operand(operand_ref, src)
                                                            : resolve_value(operand_ref, src))};
    auto&       from{type_of(value)};
    if (from.is_poison()) { return stdx::err{stop::POISON}; }
    auto&      ctx{comp_.ctx()};
    auto&      pool{comp_.constants()};
    const auto display{[&](const gs::type& t) { return ctx.type_display_name(t); }};
    const auto mismatch{[&](std::string message) -> stdx::err<stop> {
        return comp_.report(
            u_, cast_operand_src(inst, operand_ref), gs::error::TYPE_MISMATCH, std::move(message));
    }};
    const auto known{comptime_value(value)};
    if (known && pool.is_type(*known)) {
        return comp_.report(u_,
                            cast_operand_src(inst, operand_ref),
                            gs::error::TYPE_USED_AS_VALUE,
                            fmt::format("`{}` expects a value to convert, but was given a type",
                                        syntax::builtin_name(which)));
    }

    switch (which) {
    case builtin_t::BOOL_FROM_INT: {
        auto& bool_type{builtin(gs::type_kind::BOOL)};

        // Each lane of an integer vector is whether that one is non-zero
        if (const auto vec{gs::vector_of(from)}) {
            if (!gs::is_integer(vec->underlying.get_kind())) {
                return mismatch(
                    fmt::format("'@boolFromInt' expects integer lanes; found '{}'", display(from)));
            }

            auto& result_type{ctx.with_lane(from, bool_type)};
            if (const auto lanes{lanes_of(value, vec->len)}; lanes && is_comptime()) {
                std::vector<stdx::option<ip::index>> out;
                for (const auto lane : *lanes) {
                    const auto exact{int_of(pool.value_at(lane))};
                    if (!exact) {
                        return comp_.unsupported(u_, src, "a `@boolFromInt` of this lane");
                    }
                    out.emplace_back(pool.intern(!(*exact == big_int{u128{0}}), bool_type));
                }

                return operand{pool.intern(ip::aggregate_value{std::move(out)}, result_type)};
            }

            TRY(require_runtime(src));
            const auto zero{pool.intern(ip::zero_value{}, from)};
            const auto operand_value{TRY(as_runtime(value, src))};
            return gir::value{
                builder_.emit_binary(
                    instruction_kind::NE, operand_value, constant(zero, from), result_type),
                result_type};
        }

        if (!is_int_like(from) && from.get_kind() != gs::type_kind::POINTER) {
            return mismatch(fmt::format(
                "`@boolFromInt` operand must be an integer or pointer; found '{}'", display(from)));
        }

        // A runtime body compares even a known operand
        if (known && is_comptime()) {
            const auto& v{pool.value_at(*known)};
            if (const auto exact{int_of(v)}) {
                return pool.intern(!(*exact == big_int{u128{0}}), bool_type);
            }

            if (v.data.is<ip::nullptr_value>()) { return pool.intern(false, bool_type); }
            return comp_.unsupported(u_, src, "a `@boolFromInt` of a known pointer");
        }

        TRY(require_runtime(src));
        const auto operand{TRY(as_runtime(value, src))};
        const auto zero{
            from.get_kind() == gs::type_kind::POINTER
                ? constant(pool.intern(ip::nullptr_value{}, builtin(gs::type_kind::NULLPTR)),
                           builtin(gs::type_kind::NULLPTR))
                : constant(pool.intern(u64{0}, from), from)};
        return gir::value{builder_.emit_binary(instruction_kind::NE, operand, zero, bool_type),
                          bool_type};
    }
    case builtin_t::INT_FROM_PTR: {
        if (from.get_kind() != gs::type_kind::POINTER) {
            return mismatch(fmt::format("'@intFromPtr' operand must be a pointer; found '{}'{}",
                                        display(from),
                                        from.get_kind() == gs::type_kind::REFERENCE
                                            ? " (did you mean `^` instead of `&`?)"
                                            : ""));
        }

        // A known address has no number at compile time
        if (known) {
            // An address made from an integer is that integer, read at runtime in a function
            if (const auto number{int_of(pool.value_at(*known))}) {
                if (!is_comptime()) {
                    return cast_value(instruction_kind::INT_FROM_PTR,
                                      constant(*known, from),
                                      builtin(gs::type_kind::USIZE));
                }

                return operand{intern_int(pool, *number, builtin(gs::type_kind::USIZE))};
            }

            if (is_comptime()) { return comp_.needs_runtime(u_, src); }
            return comp_.unsupported(u_, src, "an `@intFromPtr` of a known address");
        }

        TRY(require_runtime(src));
        return cast_value(instruction_kind::INT_FROM_PTR,
                          TRY(as_runtime(value, src)),
                          builtin(gs::type_kind::USIZE));
    }
    case builtin_t::BACKING_INT: {
        const auto backing{ctx.backing_int_type(from, comp_.ptr_bits())};
        if (!backing) {
            return mismatch(fmt::format("`@backingInt` operand must be an enum, a packed "
                                        "struct or union, or a tagged union; found '{}'",
                                        display(from)));
        }

        // An operand a runtime body could evaluate at compile time folds too
        const auto folded{known || is_comptime() ? known : TRY(speculated_value(operand_ref, src))};
        if (folded) {
            const auto& v{pool.value_at(*folded)};
            if (const auto exact{int_of(v)}; exact && from.get_kind() == gs::type_kind::ENUM) {
                return intern_int(pool, *exact, *backing);
            }

            const auto u{v.data.as_opt<ip::union_value>()};
            const auto ut{from.get_data().as_opt<gs::types::union_t>()};
            if (u && ut) {
                const auto at{std::ranges::find(ut->field_names, u->active_field)};
                const auto tag{static_cast<u128>(at - ut->field_names.begin())};
                return intern_int(pool, big_int{tag}, *backing);
            }

            return comp_.unsupported(u_, src, "a `@backingInt` of this value at compile time");
        }

        TRY(require_runtime(src));

        // A tagged union's backing integer is its tag, read where the union lies
        if (from.get_kind() == gs::type_kind::UNION && !is_bit_packed(from)) {
            const auto base{TRY(address_of(operand_ref, src))};
            const auto tag_ptr{builder_.emit_get_element_ptr(
                base.address, {usize_constant(UNION_TAG_FIELD)}, *backing)};
            return gir::value{builder_.emit_load(gir::value{tag_ptr, *backing}, *backing),
                              *backing};
        }

        return cast_value(instruction_kind::BIT_CAST, TRY(as_runtime(value, src)), *backing);
    }
    case builtin_t::CONST_CAST:
    case builtin_t::VOLATILE_CAST: {
        const auto kind{from.get_kind()};
        const bool castable{kind == gs::type_kind::POINTER || kind == gs::type_kind::REFERENCE ||
                            kind == gs::type_kind::SLICE || kind == gs::type_kind::ARRAY};
        if (which == builtin_t::CONST_CAST && !castable) {
            return mismatch(fmt::format(
                "Expected pointer, reference, slice, or array type; found '{}'", display(from)));
        }

        auto& target{which == builtin_t::CONST_CAST ? *ctx.pool.with_const(from, false)
                                                    : *ctx.pool.strip_volatile(from)};
        if (known && is_comptime()) {
            return comp_.unsupported(u_, src, "a qualifier cast at compile time");
        }

        TRY(require_runtime(src));

        // An array is retyped where it lies, so writes through the result reach it
        if (kind == gs::type_kind::ARRAY) {
            const auto base{TRY(address_of(operand_ref, src))};
            return slot_value{.address    = gir::value{base.address.data, target},
                              .type       = &target,
                              .is_mutable = which == builtin_t::CONST_CAST};
        }

        return gir::value{TRY(as_runtime(value, src)).data, target};
    }
    default: break;
    }

    return comp_.unsupported(u_, src, fmt::format("the `{}` builtin", syntax::builtin_name(which)));
}

auto analyzer::slice_from_ptr(inst_id inst, ref ptr_ref, ref len_ref) -> result<operand> {
    const auto& src{src_of(inst)};
    const auto  ptr{TRY(resolve_value(ptr_ref, src))};
    auto&       ptr_type{type_of(ptr)};
    if (ptr_type.is_poison()) { return stdx::err{stop::POISON}; }
    const auto data{ptr_type.get_data().as_opt<gs::types::pointer>()};
    if (!data) {
        return comp_.report(u_,
                            cast_operand_src(inst, ptr_ref),
                            gs::error::TYPE_MISMATCH,
                            fmt::format("Expected a pointer-yielding expression; found '{}'",
                                        comp_.ctx().type_display_name(ptr_type)));
    }

    // The slice keeps the pointer's mutability, and no sentinel since the pointer promises none
    auto&      ctx{comp_.ctx()};
    const auto mut{ptr_type.is_constant() ? gs::types::mut::CONSTANT : gs::types::mut::MUTABLE};
    auto&      slice_type{ctx.get_slice(mut, false, data->underlying)};
    auto&      usize_type{builtin(gs::type_kind::USIZE)};
    const auto len{TRY(resolve_value(len_ref, src))};
    TRY(require_runtime(src));
    const auto ptr_value{TRY(as_runtime(ptr, src))};
    const auto len_usize{TRY(coerce_to(len, usize_type, src, {.what = use_site::kind::STORE}))};
    const auto len_value{TRY(as_runtime(len_usize, src))};
    const auto slot{builder_.emit_alloca(slice_type)};
    const auto ptr_field{builder_.emit_get_element_ptr(
        gir::value{slot, slice_type}, {usize_constant(SLICE_PTR_FIELD)}, ptr_type)};
    builder_.emit_store(gir::value{ptr_field, ptr_type}, ptr_value).is_initializer = true;
    const auto len_field{builder_.emit_get_element_ptr(
        gir::value{slot, slice_type}, {usize_constant(SLICE_LEN_FIELD)}, usize_type)};
    builder_.emit_store(gir::value{len_field, usize_type}, len_value).is_initializer = true;
    return gir::value{builder_.emit_load(gir::value{slot, slice_type}, slice_type), slice_type};
}

auto analyzer::int_cast_guard(const gir::value& value, const gs::type& target, const source& src)
    -> result<void> {
    const auto& to_lane{gs::lane_type_of(target)};
    if (!runtime_safety() || !gs::is_integer(to_lane.get_kind()) || !value.type) { return {}; }

    // A vector's lanes are each compared against splat bounds, and must all be in range
    auto&      from_type{*value.type};
    const bool is_vector{gs::is_vector(from_type)};
    auto&      from_lane{gs::lane_type_of(from_type)};
    const auto from{domain_of(from_lane)};
    const auto to{domain_of(to_lane)};
    auto&      pool{comp_.constants()};
    auto&      scalar_bool{builtin(gs::type_kind::BOOL)};
    auto&      bool_type{is_vector ? comp_.ctx().with_lane(from_type, scalar_bool) : scalar_bool};
    const auto bound{[&](const big_int& exact) {
        const auto scalar{constant(intern_int(pool, exact, from_lane), from_lane)};
        if (!is_vector) { return scalar; }
        return gir::value{builder_.emit_vector_op(instruction_kind::SPLAT, {scalar}, from_type),
                          from_type};
    }};

    // The target's smallest or largest value
    const auto limit{[&](bool upper) {
        const big_int one{u128{1}};
        const auto    span{one.shl(to.is_signed ? to.bits - 1U : to.bits)};
        const auto    exact{upper ? span - one : to.is_signed ? -span : big_int{u128{0}}};
        return bound(exact);
    }};
    const auto compare{[&](instruction_kind kind, const gir::value& bound) {
        return gir::value{builder_.emit_binary(kind, value, bound, bool_type), bool_type};
    }};
    const auto both{[&](const gir::value& a, const gir::value& b) {
        return gir::value{builder_.emit_binary(instruction_kind::AND, a, b, bool_type), bool_type};
    }};

    stdx::option<gir::value> ok;
    if (from.is_signed && to.is_signed) {
        if (from.bits > to.bits) {
            const auto lo{limit(false)};
            const auto hi{limit(true)};
            const auto ge{compare(instruction_kind::GE, lo)};
            ok.emplace(both(ge, compare(instruction_kind::LE, hi)));
        }
    } else if (!from.is_signed && !to.is_signed) {
        if (from.bits > to.bits) { ok.emplace(compare(instruction_kind::LE, limit(true))); }
    } else if (from.is_signed) {
        const auto zero{bound(big_int{u128{0}})};
        const auto non_negative{compare(instruction_kind::GE, zero)};
        if (from.bits > to.bits) {
            ok.emplace(both(non_negative, compare(instruction_kind::LE, limit(true))));
        } else {
            ok.emplace(non_negative);
        }
    } else if (from.bits >= to.bits) {
        ok.emplace(compare(instruction_kind::LE, limit(true)));
    }

    if (!ok) { return {}; }
    if (is_vector) {
        ok.emplace(builder_.emit_vector_op(instruction_kind::REDUCE,
                                           {*ok},
                                           scalar_bool,
                                           std::to_underlying(gir::reduce_op::AND)),
                   scalar_bool);
    }

    return guard(*ok, "integer value out of range for target type", src);
}

auto analyzer::enum_cast_guard(const operand&    cast,
                               const gir::value& from,
                               ref               operand_ref,
                               const source&     src) -> result<void> {
    const auto value{cast.as_opt<gir::value>()};
    if (!runtime_safety() || !value || !from.type || !gs::is_integer(from.type->get_kind())) {
        return {};
    }

    auto&      enum_type{*value->type};
    const auto en{enum_type.get_data().as_opt<gs::types::enum_t>()};
    const auto n{comp_.nominal_of(enum_type)};
    if (!en || en->non_exhaustive || !n || n->enum_values.empty()) { return {}; }

    // A value known at compile time to land on a variant needs no check
    if (const auto known{TRY(speculated_value(operand_ref, src))}) {
        const auto exact{int_of(comp_.constants().value_at(*known))};
        const auto small{exact ? exact->to_i128() : stdx::none};
        if (small && std::ranges::contains(n->enum_values, static_cast<i64>(*small))) { return {}; }
    }

    // Valid when the raw value is any variant's, compared in the enum's own integer type
    auto&                    underlying{en->underlying};
    auto&                    pool{comp_.constants()};
    auto&                    bool_type{builtin(gs::type_kind::BOOL)};
    const gir::value         raw{builder_.emit_cast(instruction_kind::BIT_CAST, *value, underlying),
                                 underlying};
    stdx::option<gir::value> ok;
    for (const auto variant : n->enum_values) {
        const auto rhs{constant(pool.intern(static_cast<u64>(variant), underlying), underlying)};
        const gir::value eq{builder_.emit_binary(instruction_kind::EQ, raw, rhs, bool_type),
                            bool_type};
        ok.emplace(ok ? gir::value{builder_.emit_binary(instruction_kind::OR, *ok, eq, bool_type),
                                   bool_type}
                      : eq);
    }

    return guard(*ok, "invalid enum value", src);
}

} // namespace ghoti::uir::sema
