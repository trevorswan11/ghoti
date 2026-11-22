#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/span>
#include <gsl/util>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/expression.hh"
#include "compiler/codegen/target.hh"
#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/layout.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/peer_type.hh"
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
    case builtin_t::BIT_SIZE_OF:              return builtin_bit_size(args, src);
    case builtin_t::TYPE_NAME:                return builtin_type_name(args, src);
    case builtin_t::TAG_NAME:                 return builtin_tag_name(args, src);
    case builtin_t::TYPE_INFO:                return builtin_type_info(args, src);
    case builtin_t::ALIGN_OF:                 return builtin_layout(args, src, false);
    case builtin_t::ASSERT:                   return builtin_assert(args, src, false);
    case builtin_t::VERIFY:                   return builtin_assert(args, src, true);
    case builtin_t::TRAP:                     return builtin_trap(src);
    case builtin_t::AS:                       return builtin_as(args, src);
    case builtin_t::THIS:                     return builtin_this(src);
    case builtin_t::INT:                      return builtin_int_type(args, src);
    case builtin_t::ENUM:                     return builtin_enum(inst, args, src);
    case builtin_t::STRUCT:                   return builtin_struct(inst, args, src);
    case builtin_t::UNION:                    return builtin_union(inst, args, src);
    case builtin_t::TARGET_OS:
    case builtin_t::TARGET_ARCH:
    case builtin_t::TARGET_ABI:
    case builtin_t::TARGET_FAMILY:            return builtin_target(which, src);
    case builtin_t::FLOAT:                    return builtin_float_type(args, src);
    case builtin_t::INT_CAST:
    case builtin_t::TRUNCATE:
    case builtin_t::BIT_CAST:
    case builtin_t::INT_FROM_BOOL:
    case builtin_t::BOOL_FROM_INT:
    case builtin_t::FROM_BACKING_INT:
    case builtin_t::BACKING_INT:
    case builtin_t::INT_FROM_FLOAT:
    case builtin_t::FLOAT_FROM_INT:
    case builtin_t::FLOAT_CAST:
    case builtin_t::PTR_CAST:
    case builtin_t::ALIGN_CAST:
    case builtin_t::PTR_FROM_INT:
    case builtin_t::INT_FROM_PTR:
    case builtin_t::CONST_CAST:
    case builtin_t::VOLATILE_CAST:
    case builtin_t::SLICE_FROM_PTR:           return builtin_cast(inst, which, args);
    default:                                  break;
    }

    return comp_.unsupported(u_, src, fmt::format("the `{}` builtin", syntax::builtin_name(which)));
}

auto analyzer::analyze_type_of(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  count{rec.count()};

    std::vector<gs::peer_operand> peers;
    std::vector<source>           sources;
    for (u32 i{0}; i < count; ++i) {
        const auto body{rec.body_field()};
        const auto value{TRY(unevaluated(body, src))};

        // A type operand's type is the type itself
        const auto known{comptime_value(value)};
        auto& type{known && comp_.constants().is_type(*known) ? comp_.constants().type_at(*known)
                                                              : type_of(value)};
        if (type.is_poison()) { return stdx::err{stop::POISON}; }

        if (!fn_ && !declaring_ && gs::is_comptime_numeric(type.get_kind())) {
            return comp_.unsupported(u_, src, "a signature's `@TypeOf` of an untyped value");
        }

        peers.emplace_back<gs::peer_operand>({.type = &type});
        sources.emplace_back(body.empty() ? src : src_of(body.back()));
    }

    auto&      ctx{comp_.ctx()};
    const auto peer{gs::peer_type(ctx, peers)};
    if (!peer) {
        return comp_.report(
            u_,
            sources[peer.error().second],
            gs::error::NO_PEER_TYPE,
            fmt::format("'@TypeOf': {}", gs::peer_error_message(ctx, peers, peer.error())));
    }

    return interned_type(**peer);
}

auto analyzer::unevaluated(gsl::span<const inst_id> body, const source& src) -> result<operand> {
    // A runtime body's operand is analyzed into a function no one emits
    if (!gir_fn_) {
        const auto out{TRY(analyze_body(body))};
        if (!out.yielded) { return comp_.unsupported(u_, src, "an operand that doesn't yield"); }
        return *out.yielded;
    }

    gir::function scratch{
        comp_.ctx().arena, comp_.constants(), std::string{}, builtin(gs::type_kind::VOID_)};
    const auto outer_fn{gir_fn_};
    const auto outer_segment{builder_.get_segment()};
    const auto restore{gsl::finally([&] {
        gir_fn_ = outer_fn;
        builder_.set_insert_point(*outer_fn, *outer_segment);
    })};
    const auto depth{std::exchange(comptime_depth_, 0)};
    const auto restore_depth{gsl::finally([&] { comptime_depth_ = depth; })};
    gir_fn_.emplace(scratch);
    builder_.set_insert_point(scratch, scratch.add_segment());
    const auto out{TRY(analyze_body(body))};
    if (!out.yielded) { return comp_.unsupported(u_, src, "an operand that doesn't yield"); }
    return *out.yielded;
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

auto analyzer::builtin_bit_size(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (args.size() != 1) { return comp_.unsupported(u_, src, "a layout builtin's arity"); }
    auto& type{*TRY(resolve_type(args.front(), src))};
    TRY(layout_ready(type, src));
    const auto layout{codegen::type_layout::resolve(comp_.ctx().target_opts.triple_str)};
    const auto ptr_bits{static_cast<u32>(layout.ptr_bytes) * 8};
    const auto kind{type.get_kind()};
    const auto bits{[&] -> u64 {
        if (kind == gs::type_kind::INT) { return gs::int_width(type); }
        if (kind == gs::type_kind::ISIZE || kind == gs::type_kind::USIZE) { return ptr_bits; }
        if (kind == gs::type_kind::BOOL) { return 1; }
        if (gs::is_float(kind)) { return gs::float_bits(kind); }
        if (const auto st{type.get_data().as_opt<gs::types::struct_t>()};
            st && st->is_bit_packed()) {
            if (const auto n{gs::packed_backing_bits(*st, ptr_bits)}) { return *n; }
        }

        if (const auto ut{type.get_data().as_opt<gs::types::union_t>()};
            ut && ut->is_bit_packed()) {
            if (const auto n{gs::packed_union_backing_bits(*ut, ptr_bits)}) { return *n; }
        }

        return static_cast<u64>(gir::size_of(type, layout)) * 8;
    }()};
    return intern_int(
        comp_.constants(), big_int{static_cast<u128>(bits)}, builtin(gs::type_kind::COMPTIME_INT));
}

auto analyzer::builtin_type_name(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (args.size() != 1) { return comp_.unsupported(u_, src, "a `@typeName` of this arity"); }
    auto&       type{*TRY(resolve_type(args.front(), src))};
    auto&       ctx{comp_.ctx()};
    std::string name{ctx.type_display_name(type)};
    auto& array{ctx.get_array(gs::types::mut::CONSTANT, true, name.size(), ctx.get_int(8, false))};
    return comp_.constants().intern(std::move(name), array);
}

auto analyzer::builtin_tag_name(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (args.size() != 1) { return comp_.unsupported(u_, src, "a `@tagName` of this arity"); }
    const auto value{TRY(resolve_value(args.front(), src))};
    auto&      ctx{comp_.ctx()};
    auto&      name_type{ctx.get_slice(gs::types::mut::CONSTANT, true, ctx.get_int(8, false))};
    if (const auto literal{value.as_opt<enum_literal>()}) {
        const auto name{mod().string(mod().data_of(literal->inst).str)};
        return comp_.constants().intern(std::string{name}, name_type);
    }

    auto&      type{type_of(value)};
    const auto tagged{type.get_data().as_opt<gs::types::union_t>()};
    if (type.get_kind() != gs::type_kind::ENUM && (!tagged || tagged->is_untagged)) {
        if (type.is_poison()) { return stdx::err{stop::POISON}; }
        return comp_.report(
            u_,
            src,
            gs::error::TYPE_MISMATCH,
            fmt::format("'@tagName' expects an enum or tagged union value; found '{}'",
                        ctx.type_display_name(type)));
    }

    const auto index{comptime_value(value)};
    if (!index) { return comp_.unsupported(u_, src, "a runtime `@tagName`"); }
    const auto& data{comp_.constants().value_at(*index).data};
    if (const auto un{data.as_opt<ip::union_value>()}) {
        return comp_.constants().intern(std::string{un->active_field}, name_type);
    }

    const auto variant{variant_name(*index)};
    if (!variant) { return comp_.unsupported(u_, src, "a `@tagName` of this value"); }
    return comp_.constants().intern(std::string{*variant}, name_type);
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
    if (self_type_) { return interned_type(*self_type_); }
    if (scope_) { return interned_type(*scope_->type); }
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

auto analyzer::builtin_assert(gsl::span<const ref> args, const source& src, bool verify)
    -> result<operand> {
    if (args.empty()) { return comp_.unsupported(u_, src, "an `@assert` without a condition"); }
    if (!is_comptime()) { return runtime_assert(args, src, verify); }
    const auto cond{TRY(resolve_value(args.front(), src))};
    if (TRY(known_bool(cond, src))) { return void_value(); }

    // Outside a call the old resolver folds the condition itself and reports it as static
    if (!comp_.in_comptime_call()) {
        const auto text{string_literal(mod(), args, 1)};
        return comp_.report(u_,
                            src,
                            gs::error::STATIC_ASSERTION_FAILED,
                            fmt::format("{} failed at compile time{}",
                                        verify ? "@verify" : "@assert",
                                        text ? fmt::format(": {}", *text) : std::string{}));
    }

    return comp_.report(
        u_,
        src,
        gs::error::COMPTIME_EVALUATION_FAILED,
        std::string{string_literal(mod(), args, 1).value_or(DEFAULT_ASSERT_FAILURE)});
}

auto analyzer::runtime_assert(gsl::span<const ref> args, const source& src, bool verify)
    -> result<operand> {
    auto&      ctx{comp_.ctx()};
    const auto cond{TRY(resolve_value(args.front(), src))};

    // A condition known to hold needs no check, and only `@verify` checks without safety
    if (const auto known{TRY(speculated_value(args.front(), src))}) {
        const auto held{comp_.constants().value_at(*known).data.as_opt<bool>()};
        if (held && *held) { return void_value(); }
    }

    if (!verify && !ctx.runtime_safety) { return void_value(); }
    const auto text{string_literal(mod(), args, 1)};
    if (args.size() > 1 && !text) {
        return comp_.unsupported(u_, src, "an `@assert` message that isn't a literal");
    }

    // The handler is emitted for the call codegen lowers the builtin to
    DISCARD(TRY(comp_.prelude_function(verify ? "panic_handler" : "assert_handler", u_, src)));
    const auto checked{TRY(condition(cond, src))};

    auto& pool{comp_.constants()};
    auto& u32_type{ctx.get_int(32, false)};
    auto& text_type{ctx.get_slice(gs::types::mut::CONSTANT, false, ctx.get_int(8, false))};
    // The old emitter locates the check at the builtin's name
    const auto callee{src.as_opt<ast::node_id>().and_then([&](ast::node_id id) {
        return u_.source->ast.get_as_opt<ast::call_expr>(id).transform(
            [](const ast::call_expr& call) { return source{ast::node_id{call.function}}; });
    })};
    const auto at{comp_.location_of(u_, callee.value_or(src)).one_based()};
    const auto message{
        text.value_or(verify ? std::string_view{"verification failed"} : "assertion failed")};
    std::vector<gir::value> call_args;
    call_args.emplace_back(checked);
    call_args.emplace_back(constant(pool.intern(u_.source->path.string(), text_type), text_type));
    call_args.emplace_back(constant(pool.intern(u64{at.line}, u32_type), u32_type));
    call_args.emplace_back(constant(pool.intern(u64{at.column}, u32_type), u32_type));
    call_args.emplace_back(constant(pool.intern(std::string{message}, text_type), text_type));
    DISCARD(builder_.emit_builtin_call(
        verify ? "@verify" : "@assert", std::move(call_args), builtin(gs::type_kind::VOID_)));
    return void_value();
}

auto analyzer::descriptor(gsl::span<const ref> args,
                          std::string_view     builtin_name,
                          std::string_view     info_name,
                          const source&        src) -> result<ip::index> {
    if (args.size() != 1) { return comp_.unsupported(u_, src, "a type builtin's arity"); }
    auto&      info{*TRY(comp_.prelude_type(info_name, u_, src))};
    const auto arg{TRY(resolve_value(args.front(), src))};
    const auto typed{TRY(coerce_to(arg, info, src, {.what = use_site::kind::STORE}))};
    const auto known{comptime_value(typed)};
    if (!known) {
        return comp_.report(
            u_,
            src,
            gs::error::COMPTIME_EVALUATION_FAILED,
            fmt::format("'{}' expects a compile-time-known descriptor argument", builtin_name));
    }

    return *known;
}

auto analyzer::builtin_int_type(gsl::span<const ref> args, const source& src) -> result<operand> {
    const auto desc{TRY(descriptor(args, "@Int", "IntInfo", src))};
    auto&      pool{comp_.constants()};
    const auto bits_field{TRY(comptime_field(desc, "bits", src))};
    const auto sign_field{TRY(comptime_field(desc, "signedness", src))};
    const auto bits_index{comptime_value(bits_field)};
    const auto sign_index{comptime_value(sign_field)};
    const auto bits{bits_index ? int_of(pool.value_at(*bits_index)) : stdx::none};
    const auto sign{sign_index ? variant_name(*sign_index) : stdx::none};
    if (!bits || !sign) { return comp_.unsupported(u_, src, "an `@Int` descriptor it can't read"); }

    const bool is_signed{*sign == "signed"};
    const auto width{bits->to_u128()};
    if (!width || *width > 65'535 || (is_signed && *width == 0)) {
        return comp_.report(
            u_,
            src,
            gs::error::LITERAL_OUT_OF_RANGE,
            fmt::format("'@Int' has no {} {}-bit integer type; widths are {}..65535",
                        *sign,
                        bits->to_string(),
                        is_signed ? 1 : 0));
    }

    return interned_type(comp_.ctx().get_int(static_cast<u16>(*width), is_signed));
}

auto analyzer::builtin_float_type(gsl::span<const ref> args, const source& src) -> result<operand> {
    const auto desc{TRY(descriptor(args, "@Float", "FloatInfo", src))};
    const auto bits_field{TRY(comptime_field(desc, "bits", src))};
    const auto bits_index{comptime_value(bits_field)};
    const auto bits{bits_index ? int_of(comp_.constants().value_at(*bits_index)) : stdx::none};
    if (!bits) { return comp_.unsupported(u_, src, "a `@Float` descriptor it can't read"); }

    const auto width{bits->to_u128().value_or(0)};
    const auto kind{[&] -> stdx::option<gs::type_kind> {
        switch (static_cast<u64>(width)) {
        case 16:  return gs::type_kind::F16;
        case 32:  return gs::type_kind::F32;
        case 64:  return gs::type_kind::F64;
        case 80:  return gs::type_kind::F80;
        case 128: return gs::type_kind::F128;
        default:  return stdx::none;
        }
    }()};
    if (!kind || width > 128) {
        return comp_.report(
            u_,
            src,
            gs::error::LITERAL_OUT_OF_RANGE,
            fmt::format("'@Float' has no {}-bit floating-point type", bits->to_string()));
    }

    return interned_type(builtin(*kind));
}

auto analyzer::builtin_target(syntax::builtin_t which, const source& src) -> result<operand> {
    const auto facts{codegen::target_facts::resolve(comp_.ctx().target_opts.triple_str)};
    const auto [enum_name, member]{[&] -> std::pair<std::string_view, std::string_view> {
        switch (which) {
        case syntax::builtin_t::TARGET_OS:   return {"Os", facts.os};
        case syntax::builtin_t::TARGET_ARCH: return {"Arch", facts.arch};
        case syntax::builtin_t::TARGET_ABI:  return {"Abi", facts.abi};
        default:                             return {"Family", facts.family};
        }
    }()};

    auto&       enum_type{*TRY(comp_.prelude_type(enum_name, u_, src))};
    const auto& en{enum_type.get_data().as<gs::types::enum_t>()};
    const bool  listed{std::ranges::contains(en.variant_names, member)};
    if (!listed) {
        return comp_.unsupported(
            u_, src, fmt::format("a target the `{}` enum doesn't list", enum_name));
    }

    return enum_constant(enum_type, member, src);
}

} // namespace ghoti::uir::sema
