#include <vector>

#include <fmt/format.h>
#include <stdx/option.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/sema/type.hh"
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
    if (is_comptime()) { return comp_.unsupported(u_, src, "a call at compile time"); }

    const auto& target{*fn->fn};
    if (args.size() != target.params.size()) {
        return comp_.report(
            u_,
            callee_src.value_or(src),
            gs::error::ARITY_MISMATCH,
            fmt::format("Expected {} arguments, found {}", target.params.size(), args.size()));
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
        values.push_back(TRY(as_runtime(coerced, src)));
    }
    TRY(require_runtime(src));

    auto&      ret{*target.return_type};
    const auto result{builder_.emit_call(target.symbol, std::move(values), ret)};
    if (ret.get_kind() == gs::type_kind::NORETURN) {
        return comp_.unsupported(u_, src, "a call to a `noreturn` function");
    }
    if (!result) { return void_value(); }
    return gir::value{*result, ret};
}

} // namespace ghoti::uir::sema
