#include <string>
#include <string_view>

#include <fmt/format.h>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/target.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/layout.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/big_int.hh"
#include "support/int128.hh"

namespace ghoti::uir::sema {

namespace {

constexpr std::string_view DEFAULT_COMPILE_ERROR{"compilation aborted by @compileError"};
constexpr std::string_view DEFAULT_ASSERT_FAILURE{"Compile-time assertion failed"};

// The text of a string literal argument, none for any other argument
[[nodiscard]] auto string_literal(const module_t& m, gsl::span<const ref> args, usize index)
    -> stdx::option<std::string_view> {
    if (args.size() <= index) { return stdx::none; }
    const auto inst{as_inst(args[index])};
    if (!inst || m.tag_of(*inst) != tag::str) { return stdx::none; }
    return m.string(m.data_of(*inst).str);
}

} // namespace

auto compile_error_message(const module_t& m, gsl::span<const ref> args) -> std::string {
    return std::string{string_literal(m, args, 0).value_or(DEFAULT_COMPILE_ERROR)};
}

auto analyzer::analyze_builtin(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto  which{static_cast<syntax::builtin_t>(rec.u32_field())};
    const auto  args{rec.refs_field()};
    const auto& src{src_of(inst)};
    using syntax::builtin_t;
    switch (which) {
    case builtin_t::COMPILE_ERROR:            return builtin_compile_error(args, src);
    case builtin_t::SET_EVAL_RECURSION_LIMIT: return builtin_limit(args, src, true);
    case builtin_t::SET_EVAL_UNROLL_LIMIT:    return builtin_limit(args, src, false);
    case builtin_t::SIZE_OF:                  return builtin_layout(args, src, true);
    case builtin_t::ALIGN_OF:                 return builtin_layout(args, src, false);
    case builtin_t::ASSERT:
    case builtin_t::VERIFY:                   return builtin_assert(args, src);
    case builtin_t::TRAP:                     return builtin_trap(src);
    case builtin_t::AS:                       return builtin_as(args, src);
    case builtin_t::THIS:                     return builtin_this(src);
    default:                                  break;
    }

    return comp_.unsupported(u_, src, fmt::format("the `{}` builtin", syntax::builtin_name(which)));
}

auto analyzer::builtin_compile_error(gsl::span<const ref> args, const source& src)
    -> result<operand> {
    if (!args.empty() && !string_literal(mod(), args, 0)) {
        return comp_.unsupported(u_, src, "a `@compileError` message that isn't a literal");
    }

    return comp_.report(
        u_, src, gs::error::COMPILE_ERROR_REACHED, compile_error_message(mod(), args));
}

auto analyzer::builtin_limit(gsl::span<const ref> args, const source& src, bool recursion)
    -> result<operand> {
    // A limit that isn't a known positive integer leaves the current one
    if (!args.empty()) {
        const auto value{resolve_value(args.front(), src)};
        const auto index{value ? comptime_value(*value) : stdx::none};
        const auto limit{index ? int_of(comp_.constants().value_at(*index)) : stdx::none};
        const auto small{limit ? limit->to_i128() : stdx::none};
        if (small && *small > 0) {
            auto& limits{comp_.limits()};
            (recursion ? limits.recursion : limits.unroll) = static_cast<usize>(*small);
        }
    }

    return void_value();
}

auto analyzer::builtin_layout(gsl::span<const ref> args, const source& src, bool size)
    -> result<operand> {
    if (args.size() != 1) { return comp_.unsupported(u_, src, "a layout builtin's arity"); }
    auto& type{*TRY(resolve_type(args.front(), src))};
    TRY(layout_ready(type, src));
    const auto  layout{codegen::type_layout::resolve(comp_.ctx().target_opts.triple_str)};
    const usize bytes{size ? gir::size_of(type, layout) : gir::align_of(type, layout)};
    return intern_int(
        comp_.constants(), big_int{static_cast<u128>(bytes)}, builtin(gs::type_kind::COMPTIME_INT));
}

auto analyzer::layout_ready(gs::type& type, const source& src) -> result<void> {
    // A type whose fields are still resolving can't be sized, and asking is a cycle
    if (const auto n{comp_.nominal_of(type)}; n && n->state == nominal_state::FIELDS_RESOLVING) {
        return comp_.report(u_,
                            src,
                            gs::error::CYCLIC_DEPENDENCY,
                            fmt::format("the layout of '{}' depends on itself", type.to_string()));
    }

    TRY(comp_.require_fields(u_, src, type));

    // Every by-value part of an aggregate must be sized too
    if (const auto st{type.get_data().as_opt<gs::types::struct_t>()}) {
        for (const auto field : st->fields) { TRY(layout_ready(*field, src)); }
    } else if (const auto ut{type.get_data().as_opt<gs::types::union_t>()}) {
        for (const auto field : ut->fields) { TRY(layout_ready(*field, src)); }
    } else if (const auto arr{type.get_data().as_opt<gs::types::array>()}) {
        TRY(layout_ready(arr->underlying, src));
    }

    return {};
}

auto analyzer::builtin_as(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (args.size() != 2) { return comp_.unsupported(u_, src, "an `@as` of this arity"); }
    auto&      target{*TRY(resolve_type(args[0], src))};
    const auto value{TRY(resolve_value(args[1], src))};
    auto&      from{type_of(value)};
    if (from.is_poison() || target.is_poison()) { return stdx::err{stop::POISON}; }
    if (!gs::is_numeric(from.get_kind()) || !gs::is_numeric(target.get_kind())) {
        return comp_.unsupported(u_, src, "an `@as` between non-numeric types");
    }

    // A negative untyped integer that the same-width signed type holds wraps into unsigned
    if (const auto index{comptime_value(value)};
        index && from.get_kind() == gs::type_kind::COMPTIME_INT &&
        gs::is_unsigned_integer(target) && domain_of(target).bits < 127) {
        const auto exact{int_of(comp_.constants().value_at(*index))};
        const auto small{exact ? exact->to_i128() : stdx::none};
        const i128 span{i128{1} << domain_of(target).bits};
        if (small && *small < 0 && *small >= -(span / 2)) {
            return operand{intern_int(comp_.constants(), big_int{*small + span}, target)};
        }
    }

    // A known typed number widens exactly into an untyped one
    if (const auto index{comptime_value(value)}; index &&
                                                 gs::is_comptime_numeric(target.get_kind()) &&
                                                 !gs::is_comptime_numeric(from.get_kind())) {
        auto&      pool{comp_.constants()};
        const auto data{pool.value_at(*index).data};
        if (target.get_kind() == gs::type_kind::COMPTIME_FLOAT && gs::is_float(from.get_kind())) {
            if (const auto f{data.as_opt<f128>()}) { return operand{pool.intern(*f, target)}; }
        }

        if (target.get_kind() == gs::type_kind::COMPTIME_INT && gs::is_integer(from.get_kind())) {
            if (const auto exact{int_of(pool.value_at(*index))}) {
                return operand{intern_int(pool, *exact, target)};
            }
        }
    }

    // A known operand converts as a value, which also checks it fits
    if (gs::is_comptime_numeric(from.get_kind()) || is_comptime()) {
        return coerce_to(value, target, src, {.what = use_site::kind::STORE});
    }

    if (!gs::is_same_unqualified(from, target) && !gs::is_implicit_widenable(from, target)) {
        return comp_.unsupported(u_, src, "an `@as` that narrows");
    }

    // Emit the cast even the value is already of the target type
    const auto operand{TRY(as_runtime(value, src))};
    return gir::value{builder_.emit_cast(gir::instruction_kind::WIDEN_CAST, operand, target),
                      target};
}

auto analyzer::builtin_this(const source& src) -> result<operand> {
    // The type whose namespace holds the declaration being analyzed
    const auto decl{fn_ ? stdx::option<decl_id>{fn_->decl} : declaring_};
    const auto owner{decl ? mod().decl_at(*decl).owner : stdx::none};
    if (!owner) { return comp_.unsupported(u_, src, "`@This` outside a type"); }
    const auto kind{[&] {
        switch (mod().tag_of(*owner)) {
        case tag::union_decl: return gs::type_kind::UNION;
        case tag::enum_decl:  return gs::type_kind::ENUM;
        default:              return gs::type_kind::STRUCT;
        }
    }()};
    if (mod().tag_of(*owner) == tag::interface_decl || mod().tag_of(*owner) == tag::impl_decl) {
        return comp_.unsupported(u_, src, "`@This` in an interface or impl");
    }

    // A member runs inside a comptime call of its own, but its type was declared outside it
    if (const auto existing{comp_.existing_nominal(u_, *owner)}) {
        return interned_type(*existing->type);
    }

    return interned_type(*comp_.declare_nominal(u_, *owner, kind).type);
}

auto analyzer::builtin_trap(const source& src) -> result<operand> {
    TRY(require_runtime(src));
    auto& noreturn_type{builtin(gs::type_kind::NORETURN)};
    DISCARD(builder_.emit_builtin_call("@trap", {}, noreturn_type));
    builder_.emit_unreachable();
    return void_value();
}

auto analyzer::builtin_assert(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (!is_comptime()) { return comp_.unsupported(u_, src, "a runtime `@assert`"); }
    if (args.empty()) { return comp_.unsupported(u_, src, "an `@assert` without a condition"); }
    const auto cond{TRY(resolve_value(args.front(), src))};
    if (!TRY(known_bool(cond, src))) {
        return comp_.report(
            u_,
            src,
            gs::error::COMPTIME_EVALUATION_FAILED,
            std::string{string_literal(mod(), args, 1).value_or(DEFAULT_ASSERT_FAILURE)});
    }

    return void_value();
}

} // namespace ghoti::uir::sema
