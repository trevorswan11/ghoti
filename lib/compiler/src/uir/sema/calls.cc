#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/ip/index.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"

namespace ghoti::uir::sema {

auto analyzer::analyze_call(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  callee_ref{*rec.ref_field()};
    const auto  modifier{rec.ref_field()};
    const auto  args{rec.refs_field()};
    const auto  pack_mask{rec.words_field()};
    if (modifier) { return comp_.unsupported(u_, src, "a call modifier"); }
    for (const auto word : pack_mask) {
        if (word != 0) { return comp_.unsupported(u_, src, "a pack expansion"); }
    }

    const auto callee_src{as_inst(callee_ref).transform([&](inst_id i) { return src_of(i); })};
    const auto callee{TRY(resolve_value(callee_ref, src))};
    const auto fn{callee.as_opt<function_ref>()};
    if (!fn) { return comp_.unsupported(u_, src, "a call through a value"); }

    return call_function(*fn->fn, args, src, callee_src.value_or(src), false);
}

auto analyzer::analyze_field_call(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  object_ref{*rec.ref_field()};
    const auto  name{rec.str_field()};
    DISCARD(rec.u32_field());
    const auto modifier{rec.ref_field()};
    const auto args{rec.refs_field()};
    const auto pack_mask{rec.words_field()};
    if (modifier) { return comp_.unsupported(u_, src, "a call modifier"); }
    for (const auto word : pack_mask) {
        if (word != 0) { return comp_.unsupported(u_, src, "a pack expansion"); }
    }

    // Only a type's own function is called through it here
    const auto object{TRY(resolve_value(object_ref, src))};
    const auto index{comptime_value(object)};
    if (!index || !comp_.constants().is_type(*index)) {
        return comp_.unsupported(u_, src, "a method call");
    }
    const auto member{TRY(member_of(comp_.constants().type_at(*index), name, src))};
    const auto fn{member.as_opt<function_ref>()};
    if (!fn) { return comp_.unsupported(u_, src, "a call through a member value"); }
    return call_function(*fn->fn, args, src, src, true);
}

auto analyzer::call_function(instance&            target,
                             gsl::span<const ref> args,
                             const source&        src,
                             const source&        callee_src,
                             bool                 through_member) -> result<operand> {
    if (args.size() != target.params.size()) {
        return comp_.report(
            u_,
            callee_src,
            gs::error::ARITY_MISMATCH,
            fmt::format("Expected {} arguments, found {}", target.params.size(), args.size()));
    }

    // A call returning `type` always runs at compile time
    if (is_comptime()) { return call_at_comptime(target, args, src); }
    if (target.comptime_only) {
        auto value{[&] {
            const auto scope{comptime_scope()};
            return call_at_comptime(target, args, src);
        }()};
        return at_comptime_boundary(std::move(value));
    }

    std::vector<gir::value> values;
    values.reserve(args.size());
    for (usize i{0}; i < args.size(); ++i) {
        const auto arg{TRY(resolve_value(args[i], src))};
        const auto coerced{TRY(coerce_to(
            arg,
            *target.params[i],
            src,
            {.what = use_site::kind::ARGUMENT, .arg_index = i + 1, .callee = target.symbol}))};
        values.emplace_back(TRY(as_runtime(coerced, src)));
    }
    TRY(require_runtime(src));

    auto& ret{*target.return_type};
    if (ret.get_kind() == gs::type_kind::NORETURN) {
        return comp_.unsupported(u_, src, "a call to a `noreturn` function");
    }

    // Call a type's member function through its name as a constant
    const auto result{
        through_member
            ? builder_.emit_indirect_call(
                  constant(comp_.constants().intern(target.symbol, *target.type), *target.type),
                  std::move(values),
                  ret)
            : builder_.emit_call(target.symbol, std::move(values), ret)};
    if (!result) { return void_value(); }
    return gir::value{*result, ret};
}

auto analyzer::call_at_comptime(instance& target, gsl::span<const ref> args, const source& src)
    -> result<operand> {
    std::vector<ip::index> values;
    values.reserve(args.size());
    for (usize i{0}; i < args.size(); ++i) {
        const auto arg{TRY(resolve_value(args[i], src))};
        const auto coerced{TRY(coerce_to(
            arg,
            *target.params[i],
            src,
            {.what = use_site::kind::ARGUMENT, .arg_index = i + 1, .callee = target.symbol}))};
        values.emplace_back(TRY(require_comptime(coerced, src)));
    }
    return comp_.call_comptime(u_, src, target, std::move(values));
}

} // namespace ghoti::uir::sema
