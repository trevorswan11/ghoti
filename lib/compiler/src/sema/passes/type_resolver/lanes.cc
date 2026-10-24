#include <algorithm>
#include <bit>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <gsl/span>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/gir/const_eval.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/generic.hh"
#include "compiler/sema/passes/type_resolver.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "gsl/pointers"
#include "internal.hh"
#include "stdx/utility.hh"
#include "support/diagnostic.hh"

namespace ghoti::sema {

using namespace resolver_detail;

namespace {

[[nodiscard]] auto is_bool_vector(const type& t, u32 len) -> bool {
    const auto vec{vector_of(t)};
    return vec && vec->len == len && vec->underlying.get_kind() == type_kind::BOOL;
}

} // namespace

auto type_resolver::resolve_arg_with_hint(const ast::call_expr::argument& arg,
                                          stdx::option<type&>             hint) -> bool {
    const structural_guard g{implicit_type_stack_, hint};
    return resolve_call_args(gsl::span<const ast::call_expr::argument>{&arg, 1}) ==
           resolve_result::OK;
}

auto type_resolver::resolve_lane_call_args(const ast::call_expr& call) -> resolve_result {
    const auto& args{call.arguments};
    const auto  kind{*call.function->get_builtin()};
    if (args.size() != (kind == syntax::builtin_t::SPLAT    ? 1U
                        : kind == syntax::builtin_t::REDUCE ? 2U
                                                            : 3U)) {
        return resolve_call_args(args);
    }

    const auto outer{implicit_type_stack_.peek()};
    const auto outer_vector{outer && is_vector(*outer) ? outer : stdx::none};
    const auto vector_arg{[&](usize i) -> stdx::option<type&> {
        auto& t{*get_call_arg_value_type(args[i])};
        return is_vector(t) ? stdx::option<type&>{t} : stdx::none;
    }};
    // A mask or predicate takes the `@Vector(N, bool)` shape of the vector beside it
    const auto bool_lanes_of{[&](stdx::option<type&> shape) -> stdx::option<type&> {
        if (!shape) { return stdx::none; }
        return ctx_.with_lane(*shape, ctx_.get_builtin_resolved_type(type_kind::BOOL));
    }};
    // The vector a memory operation reads or writes, from its pointer and the mask's length
    const auto pointee_lanes{[&](usize ptr_i, usize mask_i) -> stdx::option<type&> {
        const auto mask{vector_arg(mask_i)};
        auto&      ptr{*get_call_arg_value_type(args[ptr_i])};
        if (!mask) { return outer_vector; }
        if (const auto ptrs{vector_of(ptr)}) {
            if (const auto lane{pointee_of(ptrs->underlying)}) {
                return ctx_.with_lane(*mask, ctx_.as_lane(*lane));
            }
        } else if (const auto lane{pointee_of(ptr)}) {
            return ctx_.with_lane(*mask, ctx_.as_lane(*lane));
        }
        return outer_vector;
    }};

    switch (kind) {
    case syntax::builtin_t::SPLAT:
        return resolve_arg_with_hint(args[0],
                                     outer_vector ? stdx::option<type&>{lane_type_of(*outer_vector)}
                                                  : stdx::none)
                   ? resolve_result::OK
                   : resolve_result::POISONED;
    case syntax::builtin_t::REDUCE:
        if (!resolve_arg_with_hint(args[0], ctx_.get_builtin_type("ReduceOp")) ||
            !resolve_arg_with_hint(args[1], stdx::none)) {
            return resolve_result::POISONED;
        }
        return resolve_result::OK;
    case syntax::builtin_t::SHUFFLE: {
        if (!resolve_arg_with_hint(args[0], outer_vector) ||
            !resolve_arg_with_hint(args[1], vector_arg(0))) {
            return resolve_result::POISONED;
        }
        // A `.{ 0, 1, -1 }` mask is that many `i32`s
        stdx::option<type&> mask_hint;
        const auto          mask_expr{args[2].as_opt<ast::expr_handle>()};
        if (const auto init{mask_expr ? resolving_.ast.get_as_opt<ast::initializer_expr>(*mask_expr)
                                      : stdx::none};
            init && !init->object_type) {
            mask_hint.emplace(ctx_.get_array(
                types::mut::CONSTANT, false, init->initializers.size(), ctx_.get_int(32, true)));
        }
        return resolve_arg_with_hint(args[2], mask_hint) ? resolve_result::OK
                                                         : resolve_result::POISONED;
    }
    case syntax::builtin_t::SELECT: {
        if (!resolve_arg_with_hint(args[1], outer_vector)) { return resolve_result::POISONED; }
        auto shape{vector_arg(1)};
        if (!resolve_arg_with_hint(args[2], shape ? shape : outer_vector)) {
            return resolve_result::POISONED;
        }
        if (!shape) { shape = vector_arg(2); }
        return resolve_arg_with_hint(args[0], bool_lanes_of(shape ? shape : outer_vector))
                   ? resolve_result::OK
                   : resolve_result::POISONED;
    }
    case syntax::builtin_t::MASKED_LOAD:
    case syntax::builtin_t::GATHER:      {
        if (!resolve_arg_with_hint(args[0], stdx::none)) { return resolve_result::POISONED; }
        const auto ptrs{kind == syntax::builtin_t::GATHER ? vector_arg(0) : stdx::none};
        if (!resolve_arg_with_hint(args[1], bool_lanes_of(ptrs ? ptrs : outer_vector)) ||
            !resolve_arg_with_hint(args[2], pointee_lanes(0, 1))) {
            return resolve_result::POISONED;
        }
        return resolve_result::OK;
    }
    case syntax::builtin_t::MASKED_STORE:
    case syntax::builtin_t::SCATTER:      {
        const auto value_i{kind == syntax::builtin_t::MASKED_STORE ? 1UZ : 0UZ};
        const auto ptr_i{1UZ - value_i};
        if (!resolve_arg_with_hint(args[0], stdx::none) ||
            !resolve_arg_with_hint(args[1], stdx::none)) {
            return resolve_result::POISONED;
        }
        auto shape{vector_arg(value_i)};
        if (!shape) { shape = vector_arg(ptr_i); }
        return resolve_arg_with_hint(args[2], bool_lanes_of(shape)) ? resolve_result::OK
                                                                    : resolve_result::POISONED;
    }
    default: UNREACHABLE("Not a lane builtin");
    }
}

auto type_resolver::resolve_mul_add_args(const ast::call_expr& call) -> resolve_result {
    const auto& args{call.arguments};
    if (!resolve_arg_with_hint(args[0], stdx::none)) { return resolve_result::POISONED; }
    // Each operand is a `T`, which a `.{ ... }` initializer builds
    stdx::option<type&> operand_type;
    if (call_arg_denotes_type(args[0])) {
        operand_type.emplace(denoted_type(*get_resolved_call_arg_type(args[0])));
    }
    const auto outer{implicit_type_stack_.peek()};
    for (usize i{1}; i < args.size(); ++i) {
        const auto expr{args[i].as_opt<ast::expr_handle>()};
        const bool builds_value{expr && resolving_.ast.get_as_opt<ast::initializer_expr>(*expr)};
        if (!resolve_arg_with_hint(args[i], builds_value ? operand_type : outer)) {
            return resolve_result::POISONED;
        }
    }
    return resolve_result::OK;
}

auto type_resolver::resolve_lane_builtin(builtin_site& site) -> stdx::result<void, diagnostic> {
    const auto& call{site.call};
    const auto& args{call.arguments};
    const auto  name{syntax::builtin_name(site.kind)};
    const auto  fail{[&](usize arg, std::string message) -> stdx::result<void, diagnostic> {
        return make_sema_err(fmt::format("'{}': {}", name, std::move(message)),
                             error::TYPE_MISMATCH,
                             arg < args.size() ? get_call_arg_location(args[arg])
                                               : resolving_.ast.location_of(call.function));
    }};
    const auto  arg_type{[&](usize i) -> type& { return *get_call_arg_value_type(args[i]); }};
    const auto  shown{[&](usize i) { return ctx_.type_display_name(arg_type(i)); }};
    // The vector `arg` names; `undefined` stands for any vector of `shape`
    const auto vector_operand{[&](usize i, stdx::option<type&> shape) -> stdx::option<type&> {
        auto& t{arg_type(i)};
        if (is_vector(t)) { return t; }
        if (t.get_kind() == type_kind::UNDEFINED) { return shape; }
        return stdx::none;
    }};
    const auto require_mask{[&](usize i, u32 len) -> stdx::result<void, diagnostic> {
        if (is_bool_vector(arg_type(i), len)) { return {}; }
        return fail(
            i, fmt::format("the mask must be a '@Vector({}, bool)'; found '{}'", len, shown(i)));
    }};
    const auto result_vector{[&](u32 len, type& lane) -> gsl::not_null<type*> {
        return &ctx_.get_vector(types::mut::CONSTANT, len, ctx_.as_lane(lane));
    }};
    // Memory holds a `bool` as a byte, which a one-bit `bool` lane doesn't match
    const auto require_memory_lane{[&](usize       i,
                                       const type& lane) -> stdx::result<void, diagnostic> {
        if (lane.get_kind() != type_kind::BOOL) { return {}; }
        return fail(i,
                    "a 'bool' lane has no memory form; load 'u8' lanes and compare them instead");
    }};

    switch (site.kind) {
    case syntax::builtin_t::SPLAT: {
        const auto outer{implicit_type_stack_.peek()};
        if (!outer || !is_vector(*outer)) {
            return fail(0,
                        "needs a vector result type, as in 'let v: @Vector(4, f32) = @splat(x)'");
        }
        auto&      lane{lane_type_of(*outer)};
        auto&      scalar{arg_type(0)};
        const auto expr{args[0].as_opt<ast::expr_handle>()};
        if (is_vector(scalar) || !expr || !scalar_broadcasts(*expr, scalar, lane)) {
            return fail(0,
                        fmt::format("'{}' does not convert to the lane type '{}'",
                                    shown(0),
                                    ctx_.type_display_name(lane)));
        }
        site.return_type = result_vector(vector_of(*outer)->len, lane);
        return {};
    }
    case syntax::builtin_t::REDUCE: {
        const auto op{TRY(resolve_const_enum_arg(args[0], name, "op"))};
        const auto vec{vector_of(arg_type(1))};
        if (!vec) { return fail(1, fmt::format("expects a vector; found '{}'", shown(1))); }
        const auto& lane{vec->underlying};
        const bool  is_int{is_integer(lane.get_kind())};
        const bool  is_flt{is_float(lane.get_kind())};
        const bool  is_bool{lane.get_kind() == type_kind::BOOL};
        bool        fits{false};
        switch (static_cast<gir::reduce_op>(op.value)) {
        case gir::reduce_op::ADD:
        case gir::reduce_op::MUL:
        case gir::reduce_op::MIN:
        case gir::reduce_op::MAX: fits = is_int || is_flt; break;
        case gir::reduce_op::AND:
        case gir::reduce_op::OR:
        case gir::reduce_op::XOR: fits = is_int || is_bool; break;
        }
        if (!fits) {
            return fail(1,
                        fmt::format("'.{}' does not reduce '{}' lanes",
                                    op.name,
                                    ctx_.type_display_name(lane)));
        }
        site.return_type = &ctx_.as_lane(vec->underlying);
        return {};
    }
    case syntax::builtin_t::SHUFFLE: {
        const auto a{vector_operand(0, stdx::none)};
        if (!a) { return fail(0, fmt::format("expects a vector; found '{}'", shown(0))); }
        const auto b{vector_operand(1, a)};
        if (!b || !is_same_unqualified(lane_type_of(*a), lane_type_of(*b))) {
            return fail(1,
                        fmt::format("expects a vector of '{}' lanes; found '{}'",
                                    ctx_.type_display_name(lane_type_of(*a)),
                                    shown(1)));
        }
        const auto      mask_expr{args[2].as_opt<ast::expr_handle>()};
        gir::const_eval evaluator{ctx_, resolving_};
        const auto      folded{mask_expr ? evaluator.try_eval(*mask_expr) : stdx::none};
        const auto      mask{folded ? gir::const_eval::shuffle_mask_of(*folded) : stdx::none};
        if (!mask || mask->empty()) {
            return fail(2, "the mask must be a compile-time array of lane indices");
        }
        if (mask->size() > MAX_VECTOR_LEN) {
            return fail(2,
                        fmt::format("the mask selects {} lanes; a vector has at most {}",
                                    mask->size(),
                                    MAX_VECTOR_LEN));
        }
        const auto a_len{static_cast<i64>(vector_of(*a)->len)};
        const auto b_len{static_cast<i64>(vector_of(*b)->len)};
        for (usize i{0}; i < mask->size(); ++i) {
            const auto entry{(*mask)[i]};
            if (!entry) { continue; }
            const bool from_a{*entry >= 0};
            const auto lane{from_a ? *entry : ~*entry};
            if (lane >= (from_a ? a_len : b_len)) {
                return fail(2,
                            fmt::format("mask entry {} ({}) selects lane {} of '{}', which has {}",
                                        i,
                                        *entry,
                                        lane,
                                        from_a ? shown(0) : shown(1),
                                        from_a ? a_len : b_len));
            }
        }
        site.return_type = result_vector(static_cast<u32>(mask->size()), lane_type_of(*a));
        return {};
    }
    case syntax::builtin_t::SELECT: {
        auto&      a{arg_type(1)};
        auto&      b{arg_type(2)};
        const auto outer{implicit_type_stack_.peek()};
        auto       shape{is_vector(a)                 ? stdx::option<type&>{a}
                         : is_vector(b)               ? stdx::option<type&>{b}
                         : outer && is_vector(*outer) ? outer
                                                      : stdx::none};
        if (!shape) { return fail(1, "needs a vector operand or a vector result type"); }
        const auto len{vector_of(*shape)->len};
        auto&      lane{lane_type_of(*shape)};
        for (const usize i : {1UZ, 2UZ}) {
            auto& t{arg_type(i)};
            if (t.get_kind() == type_kind::UNDEFINED) { continue; }
            if (is_vector(t)) {
                if (vector_of(t)->len != len || !is_same_unqualified(lane_type_of(t), lane)) {
                    return fail(i,
                                fmt::format("expects a '{}'; found '{}'",
                                            ctx_.type_display_name(*shape),
                                            shown(i)));
                }
                continue;
            }
            const auto expr{args[i].as_opt<ast::expr_handle>()};
            if (!expr || !scalar_broadcasts(*expr, t, lane)) {
                return fail(i,
                            fmt::format("'{}' does not broadcast to the lanes of '{}'",
                                        shown(i),
                                        ctx_.type_display_name(*shape)));
            }
        }
        TRY(require_mask(0, len));
        site.return_type = result_vector(len, lane);
        return {};
    }
    case syntax::builtin_t::MASKED_LOAD:
    case syntax::builtin_t::GATHER:      {
        const bool is_gather{site.kind == syntax::builtin_t::GATHER};
        auto&      ptr{arg_type(0)};
        const auto ptrs{is_gather ? vector_of(ptr) : stdx::none};
        const auto lane{
            is_gather ? (ptrs ? pointee_of(ptrs->underlying) : stdx::none)
                      : (ptr.get_kind() == type_kind::POINTER ? pointee_of(ptr) : stdx::none)};
        if (!lane) {
            return fail(0,
                        fmt::format("expects {}; found '{}'",
                                    is_gather ? "a vector of pointers" : "a pointer",
                                    shown(0)));
        }
        TRY(require_memory_lane(0, *lane));
        const auto mask{vector_of(arg_type(1))};
        if (!mask) {
            return fail(1,
                        fmt::format("the mask must be a '@Vector(N, bool)'; found '{}'", shown(1)));
        }
        TRY(require_mask(1, ptrs ? ptrs->len : mask->len));
        auto&      result{*result_vector(mask->len, *lane)};
        const auto passthru{vector_operand(2, result)};
        if (!passthru || !is_assignable(*passthru, result)) {
            return fail(2,
                        fmt::format("the lanes kept where the mask is false must be a '{}'; "
                                    "found '{}'",
                                    ctx_.type_display_name(result),
                                    shown(2)));
        }
        site.return_type = &result;
        return {};
    }
    case syntax::builtin_t::MASKED_STORE:
    case syntax::builtin_t::SCATTER:      {
        const bool is_scatter{site.kind == syntax::builtin_t::SCATTER};
        const auto value_i{is_scatter ? 0UZ : 1UZ};
        const auto ptr_i{is_scatter ? 1UZ : 0UZ};
        auto&      ptr{arg_type(ptr_i)};
        const auto ptrs{is_scatter ? vector_of(ptr) : stdx::none};
        auto&      ptr_slot{ptrs ? ptrs->underlying : ptr};
        const auto lane{ptr_slot.get_kind() == type_kind::POINTER ? pointee_of(ptr_slot)
                                                                  : stdx::none};
        if (!lane || (is_scatter && !ptrs)) {
            return fail(ptr_i,
                        fmt::format("expects {}; found '{}'",
                                    is_scatter ? "a vector of pointers" : "a pointer",
                                    shown(ptr_i)));
        }
        if (ptr_slot.is_constant()) {
            return fail(ptr_i,
                        fmt::format("can't store through '{}', which points to constant memory",
                                    ctx_.type_display_name(ptr_slot)));
        }
        TRY(require_memory_lane(ptr_i, *lane));
        const auto value{vector_of(arg_type(value_i))};
        if (!value || !is_same_unqualified(value->underlying, *lane) ||
            (ptrs && ptrs->len != value->len)) {
            return fail(value_i,
                        fmt::format("expects a vector of '{}' lanes{}; found '{}'",
                                    ctx_.type_display_name(*lane),
                                    ptrs ? fmt::format(" ({} of them)", ptrs->len) : std::string{},
                                    shown(value_i)));
        }
        TRY(require_mask(2, value->len));
        site.return_type = &ctx_.get_builtin_resolved_type(type_kind::VOID_);
        return {};
    }
    default: UNREACHABLE("Not a lane builtin");
    }
}

auto type_resolver::resolve_vector_math_builtin(builtin_site& site)
    -> stdx::option<stdx::result<void, diagnostic>> {
    const auto& call{site.call};
    const auto& args{call.arguments};
    const auto  kind{site.kind};
    const bool  is_mul_add{kind == syntax::builtin_t::MUL_ADD};
    const bool  is_overflow{kind == syntax::builtin_t::ADD_WITH_OVERFLOW ||
                            kind == syntax::builtin_t::SUB_WITH_OVERFLOW ||
                            kind == syntax::builtin_t::MUL_WITH_OVERFLOW ||
                            kind == syntax::builtin_t::SHL_WITH_OVERFLOW};
    // `@mulAdd(T, a, b, c)` names its type first, and an overflow builtin's last is its result slot
    const usize first{is_mul_add ? 1UZ : 0UZ};
    const usize last{is_overflow ? std::min(args.size(), 2UZ) : args.size()};

    stdx::option<type&> shape;
    if (is_mul_add && !args.empty() && call_arg_denotes_type(args[0])) {
        if (auto& t{denoted_type(*get_resolved_call_arg_type(args[0]))}; is_vector(t)) {
            shape.emplace(t);
        }
    }
    for (usize i{first}; !shape && i < last; ++i) {
        if (auto& t{*get_call_arg_value_type(args[i])}; is_vector(t)) { shape.emplace(t); }
    }
    if (!shape) { return stdx::none; }

    const auto name{syntax::builtin_name(kind)};
    const auto fail{[&](usize arg, std::string message) -> stdx::result<void, diagnostic> {
        return make_sema_err(fmt::format("'{}': {}", name, std::move(message)),
                             error::TYPE_MISMATCH,
                             get_call_arg_location(args[arg]));
    }};
    const auto len{vector_of(*shape)->len};
    auto&      lane{lane_type_of(*shape)};

    // Every other operand is the same vector, or a scalar broadcast to its lanes
    for (usize i{first}; i < last; ++i) {
        auto& t{*get_call_arg_value_type(args[i])};
        if (t.is_poison()) { return stdx::result<void, diagnostic>{}; }
        if (is_vector(t)) {
            if (vector_of(t)->len != len || !is_same_unqualified(lane_type_of(t), lane)) {
                return fail(i,
                            fmt::format("expects a '{}'; found '{}'",
                                        ctx_.type_display_name(*shape),
                                        ctx_.type_display_name(t)));
            }
            continue;
        }
        const auto expr{args[i].as_opt<ast::expr_handle>()};
        if (!expr || !scalar_broadcasts(*expr, t, lane)) {
            return fail(i,
                        fmt::format("'{}' does not broadcast to the lanes of '{}'",
                                    ctx_.type_display_name(t),
                                    ctx_.type_display_name(*shape)));
        }
    }

    const bool is_int{is_integer(lane.get_kind())};
    const bool is_flt{is_float(lane.get_kind())};
    const auto lanes_must_be{
        [&](bool fits, std::string_view what) -> stdx::option<stdx::result<void, diagnostic>> {
            if (fits) { return stdx::none; }
            return fail(
                first,
                fmt::format("expects {} lanes; found '{}'", what, ctx_.type_display_name(*shape)));
        }};
    auto& same_shape{ctx_.get_vector(types::mut::CONSTANT, len, ctx_.as_lane(lane))};
    switch (kind) {
    case syntax::builtin_t::CLZ:
    case syntax::builtin_t::CTZ:
    case syntax::builtin_t::POPCOUNT: {
        if (auto err{lanes_must_be(is_int, "integer")}) { return err; }
        const auto bits{
            std::bit_width(static_cast<u64>(integer_effective_bits(lane, target_ptr_bits())))};
        site.return_type = &ctx_.get_vector(
            types::mut::CONSTANT, len, ctx_.get_int(static_cast<u16>(bits), false));
        return stdx::result<void, diagnostic>{};
    }
    case syntax::builtin_t::ABS:
    case syntax::builtin_t::MIN:
    case syntax::builtin_t::MAX:
    case syntax::builtin_t::MUL_ADD:
        if (auto err{lanes_must_be(is_int || is_flt, "integer or float")}) { return err; }
        break;
    case syntax::builtin_t::DIV_TRUNC:
    case syntax::builtin_t::DIV_FLOOR:
    case syntax::builtin_t::DIV_EXACT:
    case syntax::builtin_t::REM:
    case syntax::builtin_t::MOD:
        if (auto err{lanes_must_be(is_int, "integer")}) { return err; }
        break;
    case syntax::builtin_t::ADD_WITH_OVERFLOW:
    case syntax::builtin_t::SUB_WITH_OVERFLOW:
    case syntax::builtin_t::MUL_WITH_OVERFLOW:
    case syntax::builtin_t::SHL_WITH_OVERFLOW: {
        if (auto err{lanes_must_be(is_int, "integer")}) { return err; }
        auto&      slot{*get_resolved_call_arg_type(args[2])};
        const auto out{pointee_of(slot)};
        if (!out || slot.is_constant() || !is_same_unqualified(*out, same_shape)) {
            return fail(2,
                        fmt::format("expects its third argument to be a '&mut {}' result "
                                    "reference; found '{}'",
                                    ctx_.type_display_name(same_shape),
                                    ctx_.type_display_name(slot)));
        }
        site.return_type = &ctx_.get_vector(
            types::mut::CONSTANT, len, ctx_.get_builtin_resolved_type(type_kind::BOOL));
        return stdx::result<void, diagnostic>{};
    }
    default:
        // `@sqrt` and the other float functions
        if (auto err{lanes_must_be(is_flt, "float")}) { return err; }
        break;
    }
    site.return_type = &same_shape;
    return stdx::result<void, diagnostic>{};
}

auto type_resolver::resolve_vector_cast_builtin(builtin_site& site)
    -> stdx::option<stdx::result<void, diagnostic>> {
    const auto& call{site.call};
    const auto  kind{site.kind};
    const auto  name{syntax::builtin_name(kind)};
    const auto  result{[&](type& t) {
        site.return_type = &t;
        return stdx::result<void, diagnostic>{};
    }};

    // `@boolFromInt(v)` takes no target: each lane is whether that one is non-zero
    if (kind == syntax::builtin_t::BOOL_FROM_INT) {
        auto& src{*get_call_arg_value_type(call.arguments[0])};
        if (!is_vector(src)) { return stdx::none; }
        if (!is_integer(lane_type_of(src).get_kind())) {
            return make_sema_err(fmt::format("'{}' expects integer lanes; found '{}'",
                                             name,
                                             ctx_.type_display_name(src)),
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(call.arguments[0]));
        }
        return result(ctx_.with_lane(src, ctx_.get_builtin_resolved_type(type_kind::BOOL)));
    }

    const bool lane_cast{
        kind == syntax::builtin_t::INT_CAST || kind == syntax::builtin_t::TRUNCATE ||
        kind == syntax::builtin_t::INT_FROM_BOOL || kind == syntax::builtin_t::INT_FROM_FLOAT ||
        kind == syntax::builtin_t::FLOAT_FROM_INT || kind == syntax::builtin_t::FLOAT_CAST};
    if (!lane_cast) { return stdx::none; }
    // The scalar path reports a missing or ill-formed target
    const auto args{cast_args_of(call, name)};
    if (!args || !args->target || args->target->is_poison()) { return stdx::none; }
    auto& target{*args->target};
    auto& src{*get_call_arg_value_type(*args->operand)};
    if (src.is_poison() || (!is_vector(target) && !is_vector(src))) { return stdx::none; }

    const auto target_vec{vector_of(target)};
    const auto src_vec{vector_of(src)};
    if (!target_vec || !src_vec || target_vec->len != src_vec->len) {
        return make_sema_err(fmt::format("'{}' converts a vector to a vector of as many lanes; "
                                         "found '{}' to '{}'",
                                         name,
                                         ctx_.type_display_name(src),
                                         ctx_.type_display_name(target)),
                             error::TYPE_MISMATCH,
                             get_call_arg_location(*args->operand));
    }

    const auto& to{target_vec->underlying};
    const auto& from{src_vec->underlying};
    const bool  to_int{is_integer(to.get_kind())};
    const bool  to_flt{is_float(to.get_kind())};
    const bool  from_int{is_integer(from.get_kind())};
    const bool  from_flt{is_float(from.get_kind())};
    const auto  ptr_bits{target_ptr_bits()};
    const bool  fits{[&] {
        switch (kind) {
        case syntax::builtin_t::INT_CAST: return to_int && from_int;
        case syntax::builtin_t::TRUNCATE:
            return to_int && from_int &&
                   integer_effective_bits(to, ptr_bits) <= integer_effective_bits(from, ptr_bits);
        case syntax::builtin_t::INT_FROM_BOOL:  return to_int && from.get_kind() == type_kind::BOOL;
        case syntax::builtin_t::INT_FROM_FLOAT: return to_int && from_flt;
        case syntax::builtin_t::FLOAT_FROM_INT: return to_flt && from_int;
        default:                                return to_flt && from_flt; // `@floatCast`
        }
    }()};
    if (!fits) {
        return make_sema_err(fmt::format("'{}' can't convert '{}' lanes to '{}' lanes",
                                         name,
                                         ctx_.type_display_name(from),
                                         ctx_.type_display_name(to)),
                             error::TYPE_MISMATCH,
                             get_call_arg_location(*args->operand));
    }
    return result(target);
}

auto type_resolver::dependent_vector_param(const generic_function_info& fn_info,
                                           usize                        idx,
                                           const ast::call_expr&        call,
                                           const expanded_call_args&    expanded)
    -> stdx::option<type&> {
    const auto& params{fn_info.fn_expr->parameters};
    const auto& tree{fn_info.module->ast};
    if (idx >= params.size() || !params[idx].explicit_type) { return stdx::none; }
    const auto vector_call{tree.get_as_opt<ast::call_expr>(*params[idx].explicit_type)};
    if (!vector_call || vector_call->function->get_builtin() != syntax::builtin_t::VECTOR) {
        return stdx::none;
    }

    // The earlier `comptime` parameters, as this call's arguments bind them
    comptime_frame frame;
    for (usize j{0}; j < idx && j < expanded.source_index.size(); ++j) {
        if (!params[j].is_comptime || !params[j].name.is<ast::identifier_expr>()) { continue; }
        auto folded{fold_implicit_comptime_arg(call.arguments[expanded.source_index[j]])};
        if (!folded) { return stdx::none; }
        frame.insert_or_assign(tree.get_as<ast::identifier_expr>(params[j].name).name,
                               std::move(*folded));
    }
    const comptime_frame_guard bound{ctx_.comptime_binding_frames, std::move(frame)};
    gir::const_eval            evaluator{ctx_, *fn_info.module};
    const auto                 diags_before{ctx_.diags.size()};
    const auto                 vec{evaluator.eval_vector_type(*vector_call)};
    DISCARD(ctx_.diags.split_off(diags_before));
    if (!vec) { return stdx::none; }
    return **vec;
}

} // namespace ghoti::sema
