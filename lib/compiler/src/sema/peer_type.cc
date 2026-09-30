#include "compiler/sema/peer_type.hh"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/sema/context.hh"
#include "compiler/sema/type.hh"

namespace ghoti::sema {

namespace {

using peer_result = stdx::result<gsl::not_null<type*>, peer_error>;

// Whether a `from` value becomes a `to` without a cast
[[nodiscard]] auto converts_to(const type& from, const type& to) noexcept -> bool {
    const auto from_kind{from.get_kind()};
    const auto to_kind{to.get_kind()};
    if (is_numeric(from_kind) && is_numeric(to_kind)) {
        // A concrete number never becomes an untyped constant
        if (is_constexpr_numeric(to_kind) && !is_constexpr_numeric(from_kind)) { return false; }
        return is_same_unqualified(from, to) || is_implicit_widenable(from, to);
    }
    if (is_numeric(from_kind) || is_numeric(to_kind)) { return false; }
    if (from_kind != to_kind && from_kind != type_kind::NULLPTR) { return false; }
    return is_assignable(from, to);
}

[[nodiscard]] auto pointee_of(const type& t) noexcept -> stdx::option<type&> {
    if (const auto ptr{t.get_data().as_opt<types::pointer>()}) { return ptr->underlying; }
    if (const auto ref{t.get_data().as_opt<types::reference>()}) { return ref->underlying; }
    return stdx::none;
}

// A pointer or reference peer stays `volatile` when any operand points at volatile storage
[[nodiscard]] auto keep_volatile(context& ctx, type& peer, gsl::span<const type* const> operands)
    -> type& {
    const auto pointee{pointee_of(peer)};
    if (!pointee || pointee->is_volatile()) { return peer; }
    const bool any_volatile{std::ranges::any_of(operands, [](const type* operand) {
        const auto target{pointee_of(*operand)};
        return target && target->is_volatile();
    })};
    if (!any_volatile) { return peer; }
    auto&      volatile_pointee{*ctx.pool.with_volatile(*pointee, true)};
    const auto mutability{peer.is_constant() ? types::mut::CONSTANT : types::mut::MUTABLE};
    return peer.get_kind() == type_kind::POINTER ? ctx.get_pointer(mutability, volatile_pointee)
                                                 : ctx.get_reference(mutability, volatile_pointee);
}

struct sequence_shape {
    type& element;
    bool  null_terminated;
    bool  is_array_value;
};

// An array, a reference to one, or a slice: everything that converts to a slice. A pointer to an
// array doesn't, so it is no peer of one either
[[nodiscard]] auto sequence_shape_of(type& t) noexcept -> stdx::option<sequence_shape> {
    if (const auto arr{t.get_data().as_opt<types::array>()}) {
        return sequence_shape{arr->underlying, arr->null_terminated, true};
    }
    if (const auto slice{t.get_data().as_opt<types::slice>()}) {
        return sequence_shape{slice->underlying, slice->null_terminated, false};
    }
    if (const auto ref{t.get_data().as_opt<types::reference>()}) {
        if (const auto arr{ref->underlying.get_data().as_opt<types::array>()}) {
            return sequence_shape{arr->underlying, arr->null_terminated, false};
        }
    }
    return stdx::none;
}

[[nodiscard]] auto slice_peer(context&                      ctx,
                              gsl::span<const peer_operand> operands,
                              gsl::span<const usize>        live) -> stdx::option<peer_result> {
    stdx::option<sequence_shape> first;
    bool                         any_const{false};
    bool                         all_terminated{true};
    for (const auto index : live) {
        const auto shape{sequence_shape_of(*operands[index].type)};
        if (!shape) { return stdx::none; }
        if (first && !is_same_unqualified(first->element, shape->element)) { return stdx::none; }
        if (!first) { first.emplace(*shape); }
        any_const      = any_const || shape->element.is_constant();
        all_terminated = all_terminated && shape->null_terminated;
    }
    if (!first) { return stdx::none; }

    for (const auto index : live) {
        const auto shape{sequence_shape_of(*operands[index].type)};
        if (shape->is_array_value && !operands[index].addressable) {
            return peer_result{stdx::err{peer_error{
                .first  = live.front(),
                .second = index,
                .hint   = "bind the array to a name first so a slice can point at it",
            }}};
        }
    }
    auto& element{*ctx.pool.with_const(first->element, any_const)};
    return peer_result{
        gsl::not_null<type*>{&ctx.get_slice(types::mut::CONSTANT, all_terminated, element)}};
}

[[nodiscard]] auto cast_hint(const type& a, const type& b) -> std::string {
    const auto a_kind{a.get_kind()};
    const auto b_kind{b.get_kind()};
    const auto is_floating{
        [](type_kind kind) { return is_float(kind) || is_constexpr_float(kind); }};
    if (is_integer(a_kind) && is_integer(b_kind)) { return "convert one with `@intCast`"; }
    if ((is_integer(a_kind) && is_floating(b_kind)) ||
        (is_floating(a_kind) && is_integer(b_kind))) {
        return "convert the integer with `@floatFromInt`";
    }
    return {};
}

} // namespace

auto peer_type(context& ctx, gsl::span<const peer_operand> operands) -> peer_result {
    if (operands.empty()) {
        return gsl::not_null<type*>{&ctx.get_builtin_resolved_type(type_kind::VOID_)};
    }

    // A reference among plain values stands for the value it refers to
    const auto is_reference{[](const peer_operand& operand) {
        return operand.type->get_kind() == type_kind::REFERENCE;
    }};
    const auto takes_part{[](const peer_operand& operand) {
        const auto kind{operand.type->get_kind()};
        return kind != type_kind::NORETURN && kind != type_kind::UNDEFINED;
    }};
    const bool mixes_references{std::ranges::any_of(operands, is_reference) &&
                                std::ranges::any_of(operands, [&](const peer_operand& operand) {
                                    return takes_part(operand) && !is_reference(operand);
                                })};
    if (mixes_references) {
        std::vector<peer_operand> values{operands.begin(), operands.end()};
        for (auto& value : values) {
            if (const auto ref{value.type->get_data().as_opt<types::reference>()}) {
                value.type = &ref->underlying;
            }
        }
        return peer_type(ctx, values);
    }

    std::vector<usize>       live;
    std::vector<const type*> live_types;
    stdx::option<type&>      placeholder;
    for (usize i{0}; i < operands.size(); ++i) {
        auto& t{*operands[i].type};
        if (t.is_poison()) { return gsl::not_null<type*>{&ctx.get_poison()}; }
        if (t.get_kind() == type_kind::NORETURN) { continue; }
        // `undefined` takes whatever type the other operands settle on
        if (t.get_kind() == type_kind::UNDEFINED) {
            if (!placeholder) { placeholder.emplace(t); }
            continue;
        }
        live.push_back(i);
        live_types.push_back(&t);
    }
    if (live.empty()) {
        return gsl::not_null<type*>{placeholder ? placeholder.get() : operands.front().type.get()};
    }

    const bool only_nullptr{std::ranges::all_of(
        live_types, [](const type* t) { return t->get_kind() == type_kind::NULLPTR; })};
    for (const auto candidate : live) {
        auto& target{*operands[candidate].type};
        if (target.get_kind() == type_kind::NULLPTR && !only_nullptr) { continue; }
        const bool all_convert{std::ranges::all_of(live, [&](usize index) {
            return index == candidate || converts_to(*operands[index].type, target);
        })};
        if (all_convert) { return gsl::not_null<type*>{&keep_volatile(ctx, target, live_types)}; }
    }

    if (auto slice{slice_peer(ctx, operands, live)}) { return std::move(*slice); }

    // Name the first operand and the first one it shares no type with
    const auto first{live.front()};
    auto       second{live.back()};
    for (const auto index : live) {
        if (index != first && !is_same_unqualified(*operands[first].type, *operands[index].type)) {
            second = index;
            break;
        }
    }
    return stdx::err{peer_error{
        .first  = first,
        .second = second,
        .hint   = cast_hint(*operands[first].type, *operands[second].type),
    }};
}

auto peer_error_message(const context&                ctx,
                        gsl::span<const peer_operand> operands,
                        const peer_error&             error) -> std::string {
    auto message{fmt::format("no peer type for '{}' and '{}'",
                             ctx.type_display_name(*operands[error.first].type),
                             ctx.type_display_name(*operands[error.second].type))};
    if (!error.hint.empty()) { message += fmt::format("; {}", error.hint); }
    return message;
}

} // namespace ghoti::sema
