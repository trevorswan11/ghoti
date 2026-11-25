#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/codegen/target.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/float128.hh"

namespace ghoti::uir::sema {

namespace {

using syntax::builtin_t;

[[nodiscard]] auto is_memory_order(std::string_view name) noexcept -> bool {
    return name == "relaxed" || name == "acquire" || name == "release" || name == "acq_rel" ||
           name == "seq_cst";
}

// A total order over `MemoryOrder` (C++11 ranking)
[[nodiscard]] auto order_rank(std::string_view name) noexcept -> i32 {
    if (name == "relaxed") { return 0; }
    if (name == "acquire" || name == "release") { return 1; }
    if (name == "acq_rel") { return 2; }
    if (name == "seq_cst") { return 3; }
    return -1;
}

[[nodiscard]] auto is_rmw_op(std::string_view name) noexcept -> bool {
    static constexpr std::array names{
        "xchg", "add", "sub", "band", "nand", "bor", "bxor", "max", "min", "umax", "umin"};
    return std::ranges::contains(names, name);
}

} // namespace

auto analyzer::atomic_enum_arg(builtin_t            which,
                               gsl::span<const ref> args,
                               usize                index,
                               std::string_view     prelude,
                               std::string_view     what,
                               const source&        src) -> result<atomic_choice> {
    const auto name{syntax::builtin_name(which)};
    const auto at{arg_src(src, index).value_or(src)};
    auto&      type{*TRY(comp_.prelude_type(prelude, u_, src))};
    const auto given{TRY(resolve_value(args[index], src))};
    const auto value{[&]() -> result<stdx::option<ip::index>> {
        const auto scope{comptime_scope()};
        return comptime_value(TRY(coerce_to(given, type, at, {.what = use_site::kind::STORE})));
    }()};
    if (!value && value.error() != stop::RUNTIME) { return stdx::err{value.error()}; }
    const auto known{value ? *value : stdx::none};
    const auto variant{known ? variant_name(*known) : stdx::none};
    const auto number{known ? int_of(comp_.constants().value_at(*known)) : stdx::none};
    if (!variant || !number) {
        return comp_.report(
            u_,
            at,
            gs::error::TYPE_MISMATCH,
            fmt::format("'{}' expects '{}' to be a compile-time constant", name, what));
    }

    return atomic_choice{
        .name = std::string{*variant}, .value = static_cast<u8>(*number->to_i128()), .at = at};
}

auto analyzer::builtin_atomic(builtin_t which, gsl::span<const ref> args, const source& src)
    -> result<operand> {
    const auto name{syntax::builtin_name(which)};
    auto&      ctx{comp_.ctx()};
    const auto display{[&](const gs::type& t) { return ctx.type_display_name(t); }};
    const auto fail{[&](const source& at, std::string message) -> stdx::err<stop> {
        return comp_.report(u_, at, gs::error::TYPE_MISMATCH, std::move(message));
    }};
    const auto arity{[&] -> usize {
        switch (which) {
        case builtin_t::FENCE:          return 1;
        case builtin_t::ATOMIC_RMW:     return 5;
        case builtin_t::CMPXCHG_WEAK:
        case builtin_t::CMPXCHG_STRONG: return 7;
        default:                        return 3;
        }
    }()};
    if (args.size() != arity) { return comp_.unsupported(u_, src, "an atomic builtin's arity"); }
    auto&      void_type{builtin(gs::type_kind::VOID_)};
    const auto order_arg{[&](usize i, std::string_view what) {
        return atomic_enum_arg(which, args, i, "MemoryOrder", what, src);
    }};

    if (which == builtin_t::FENCE) {
        const auto order{TRY(order_arg(0, "order"))};
        if (!is_memory_order(order.name)) {
            return fail(order.at,
                        fmt::format("'{}' cannot use memory order '{}'; a fence accepts relaxed, "
                                    "acquire, release, acq_rel, or seq_cst",
                                    name,
                                    order.name));
        }

        TRY(require_runtime(src));
        DISCARD(builder_.emit_builtin_call(name, {}, void_type, stdx::none, order.value));
        return void_value();
    }

    // `@atomicStore` names no `T`, which is the pointee of its `ptr`
    const bool              has_type{which != builtin_t::ATOMIC_STORE};
    const usize             ptr_i{has_type ? 1UZ : 0UZ};
    const auto              ptr_at{arg_src(src, ptr_i).value_or(src)};
    stdx::option<gs::type&> named;
    if (has_type) { named.emplace(*TRY(resolve_type(args[0], src))); }
    const auto ptr{TRY(resolve_value(args[ptr_i], src))};
    auto&      ptr_type{type_of(ptr)};
    const auto pointer{ptr_type.get_data().as_opt<gs::types::pointer>()};
    if (!pointer) {
        return fail(ptr_at,
                    fmt::format("'{}' expects 'ptr' to be a pointer ('^T' / '^mut T'); found '{}'",
                                name,
                                display(ptr_type)));
    }

    auto& t{named ? *named : pointer->underlying};
    if (which != builtin_t::ATOMIC_LOAD && ptr_type.is_constant()) {
        return fail(ptr_at,
                    fmt::format("'{}' expects 'ptr' to point to a mutable location ('^mut {}'); "
                                "found '{}'",
                                name,
                                display(t),
                                display(ptr_type)));
    }

    if (!gs::is_same_unqualified(pointer->underlying, t)) {
        return fail(ptr_at,
                    fmt::format("'{}' expects 'ptr' to point to '{}'; found '{}'",
                                name,
                                display(t),
                                display(ptr_type)));
    }

    // Only the widths the target can access atomically
    const auto width{[&]() -> stdx::option<u32> {
        switch (t.get_kind()) {
        case gs::type_kind::INT:
        case gs::type_kind::ISIZE:
        case gs::type_kind::USIZE:   return domain_of(t).bits;
        case gs::type_kind::POINTER: return comp_.ptr_bits();
        case gs::type_kind::BOOL:    return 8U;
        default:
            if (const auto format{gs::float_format_of(t)}) {
                return format_info(*format).storage_bits;
            }
            return stdx::none;
        }
    }()};
    const auto arch{codegen::target_facts::resolve(ctx.target_opts.triple_str).arch};
    const bool wide_ok{arch == "x86_64" || arch == "aarch64"};
    if (!width || (*width != 8 && *width != 16 && *width != 32 && *width != 64 &&
                   (*width != 128 || !wide_ok))) {
        return fail(ptr_at,
                    fmt::format("'{}' operand type '{}' has no native atomic width on this target; "
                                "8/16/32/64 bits are always supported, 128 only on x86_64/aarch64",
                                name,
                                display(t)));
    }

    // Every value operand is a `T`, an untyped number taking it
    const auto operand_as_t{[&](usize i, std::string_view what) -> result<gir::value> {
        const auto at{arg_src(src, i).value_or(src)};
        const auto given{TRY(resolve_value(args[i], src))};
        auto&      from{type_of(given)};
        if (const auto known{comptime_value(given)}; known && comp_.constants().is_type(*known)) {
            return comp_.report(
                u_,
                at,
                gs::error::TYPE_USED_AS_VALUE,
                fmt::format("'{}' expects '{}' to be a value, but was given a type", name, what));
        }

        const bool fits{
            gs::is_same_unqualified(from, t) ||
            (from.get_kind() == gs::type_kind::COMPTIME_INT && gs::is_integer(t.get_kind())) ||
            (from.get_kind() == gs::type_kind::COMPTIME_FLOAT && gs::is_float(t.get_kind()))};
        if (!fits) {
            return fail(at,
                        fmt::format("'{}' expects '{}' to be '{}'; found '{}'",
                                    name,
                                    what,
                                    display(t),
                                    display(from)));
        }

        return as_runtime(TRY(coerce_to(given, t, at, {.what = use_site::kind::STORE})), src);
    }};

    switch (which) {
    case builtin_t::ATOMIC_LOAD: {
        const auto order{TRY(order_arg(2, "order"))};
        if (order.name != "relaxed" && order.name != "acquire" && order.name != "seq_cst") {
            return fail(order.at,
                        fmt::format("'{}' cannot use memory order '{}'; loads accept relaxed, "
                                    "acquire, or seq_cst",
                                    name,
                                    order.name));
        }

        TRY(require_runtime(src));
        const auto address{TRY(as_runtime(ptr, src))};
        const auto loaded{builder_.emit_builtin_call(name, {address}, t, stdx::none, order.value)};
        return operand{gir::value{*loaded, t}};
    }
    case builtin_t::ATOMIC_STORE: {
        const auto stored{TRY(operand_as_t(1, "val"))};
        const auto order{TRY(order_arg(2, "order"))};
        if (order.name != "relaxed" && order.name != "release" && order.name != "seq_cst") {
            return fail(order.at,
                        fmt::format("'{}' cannot use memory order '{}'; stores accept relaxed, "
                                    "release, or seq_cst",
                                    name,
                                    order.name));
        }

        TRY(require_runtime(src));
        const auto address{TRY(as_runtime(ptr, src))};
        DISCARD(builder_.emit_builtin_call(
            name, {address, stored}, void_type, stdx::none, order.value));
        return void_value();
    }
    case builtin_t::ATOMIC_RMW: {
        const auto op{TRY(atomic_enum_arg(which, args, 2, "AtomicRmwOp", "op", src))};
        if (!is_rmw_op(op.name)) {
            return fail(op.at,
                        fmt::format("'{}' has no atomic RMW operation named '{}'", name, op.name));
        }

        const auto operand_value{TRY(operand_as_t(3, "val"))};
        const auto order{TRY(order_arg(4, "order"))};
        if (!is_memory_order(order.name)) {
            return fail(order.at,
                        fmt::format("'{}' cannot use memory order '{}'", name, order.name));
        }

        TRY(require_runtime(src));
        const auto address{TRY(as_runtime(ptr, src))};
        const auto old{
            builder_.emit_builtin_call(name, {address, operand_value}, t, op.value, order.value)};
        return operand{gir::value{*old, t}};
    }
    default: break;
    }

    // `@cmpxchgWeak` and `@cmpxchgStrong` write the value they found through `out`
    const auto expected{TRY(operand_as_t(2, "expected"))};
    const auto desired{TRY(operand_as_t(3, "new"))};
    const auto success{TRY(order_arg(4, "succ_order"))};
    if (!is_memory_order(success.name)) {
        return fail(success.at,
                    fmt::format("'{}' cannot use success memory order '{}'", name, success.name));
    }

    const auto failure{TRY(order_arg(5, "fail_order"))};
    if (failure.name == "release" || failure.name == "acq_rel") {
        return fail(failure.at,
                    fmt::format("'{}' cannot use failure memory order '{}'; 'fail_order' may not "
                                "be release or acq_rel",
                                name,
                                failure.name));
    }

    if (!is_memory_order(failure.name) || order_rank(failure.name) > order_rank(success.name)) {
        return fail(failure.at,
                    fmt::format("'{}' failure memory order '{}' may not be stronger than its "
                                "success order '{}'",
                                name,
                                failure.name,
                                success.name));
    }

    const auto out_at{arg_src(src, 6).value_or(src)};
    const auto out{TRY(resolve_value(args[6], src))};
    auto&      out_type{type_of(out)};
    const auto out_pointee{gs::pointee_of(out_type)};
    if (!out_pointee || out_type.is_constant() || !gs::is_same_unqualified(*out_pointee, t)) {
        return fail(out_at,
                    fmt::format("'{}' expects its 'out' argument to be a '&mut {}' result "
                                "reference; found '{}'",
                                name,
                                display(t),
                                display(out_type)));
    }

    TRY(require_runtime(src));
    auto&      bool_type{builtin(gs::type_kind::BOOL)};
    const auto address{TRY(as_runtime(ptr, src))};
    const auto out_address{TRY(as_runtime(out, src))};
    const auto swapped{builder_.emit_builtin_call(name,
                                                  {address, expected, desired, out_address},
                                                  bool_type,
                                                  stdx::none,
                                                  success.value,
                                                  failure.value)};
    return operand{gir::value{*swapped, bool_type}};
}

} // namespace ghoti::uir::sema
