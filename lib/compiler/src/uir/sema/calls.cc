#include <algorithm>
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
    const auto callee_src{as_inst(callee_ref).transform([&](inst_id i) { return src_of(i); })};
    const auto callee{TRY(resolve_value(callee_ref, src))};
    const bool expands{std::ranges::any_of(pack_mask, [](u32 word) { return word != 0; })};

    // `.name(args)` calls the member of whatever type the call is used as
    if (callee.is<enum_literal>() && !expands) { return implicit_call{inst}; }
    if (!expands) { return call_operand(callee, args, src, callee_src.value_or(src)); }

    // `f(rest...)` passes each of the pack's values as an argument of its own
    const auto generic{callee.as_opt<generic_ref>()};
    if (!generic) { return comp_.unsupported(u_, src, "a pack expansion into this callee"); }
    std::vector<call_arg> values;
    for (usize i{0}; i < args.size(); ++i) {
        const auto value{TRY(resolve_value(args[i], src))};
        const bool spread{(pack_mask[i / 32] & (1U << (i % 32))) != 0};
        const auto pack{value.as_opt<pack_ref>()};
        if (!spread) {
            values.emplace_back(call_arg{.value = value, .written = args[i]});
            continue;
        }

        if (!pack) { return comp_.unsupported(u_, src, "an expansion of something not a pack"); }
        for (const auto& element : packs_[pack->index]) {
            values.emplace_back(call_arg{.value = element, .written = stdx::none});
        }
    }

    return call_generic(*generic->fn, std::move(values), src, callee_src.value_or(src));
}

auto analyzer::call_operand(const operand&       callee,
                            gsl::span<const ref> args,
                            const source&        src,
                            const source&        callee_src) -> result<operand> {
    if (const auto generic{callee.as_opt<generic_ref>()}) {
        std::vector<call_arg> values;
        for (const auto arg : args) {
            values.emplace_back(call_arg{.value = TRY(resolve_value(arg, src)), .written = arg});
        }

        return call_generic(*generic->fn, std::move(values), src, callee_src);
    }

    const auto fn{callee.as_opt<function_ref>()};
    if (!fn) { return comp_.unsupported(u_, src, "a call through a value"); }

    return call_function(*fn->fn, args, src, callee_src, false);
}

auto analyzer::analyze_field_call(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  object_ref{*rec.ref_field()};
    const auto  name{rec.str_field()};
    rec.skip_u32();
    const auto modifier{rec.ref_field()};
    const auto args{rec.refs_field()};
    const auto pack_mask{rec.words_field()};
    if (modifier) { return comp_.unsupported(u_, src, "a call modifier"); }
    for (const auto word : pack_mask) {
        if (word != 0) { return comp_.unsupported(u_, src, "a pack expansion"); }
    }

    const auto object{TRY(resolve(object_ref, src))};

    // A module's function is called like one declared here
    if (const auto m{object.as_opt<module_ref>()}) {
        const auto member{TRY(module_member(*m, name, src))};
        return call_operand(member, args, src, src);
    }

    const auto index{comptime_value(object)};
    if (!index || !comp_.constants().is_type(*index)) {
        if (const auto through_vtable{TRY(call_dyn(object, name, args, src))}) {
            return *through_vtable;
        }

        const auto owner{receiver_type(object)};
        if (!owner) { return comp_.unsupported(u_, src, "a call through a field"); }
        return call_method(*owner, object_ref, name, args, src);
    }

    auto&      owner{comp_.constants().type_at(*index)};
    const auto member{TRY(member_of(owner, name, src))};
    if (const auto generic{member.as_opt<generic_ref>()}) {
        std::vector<call_arg> values;
        for (const auto arg : args) {
            values.emplace_back(call_arg{.value = TRY(resolve_value(arg, src)), .written = arg});
        }

        return call_generic(*generic->fn, std::move(values), src, src);
    }

    const auto fn{member.as_opt<function_ref>()};
    if (!fn) { return comp_.unsupported(u_, src, "a call through a member value"); }

    // A plain type's static function is called by name, a ctor instance's or a method directly
    const auto n{comp_.nominal_of(owner)};
    const bool by_name{!fn->fn->self_modifier && !(n && n->in_call)};
    return call_function(*fn->fn, args, src, src, by_name);
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

    std::vector<operand> values;
    values.reserve(args.size());
    for (const auto arg : args) { values.emplace_back(TRY(resolve_value(arg, src))); }
    return call_following_poly(target, std::move(values), src, through_member);
}

auto analyzer::call_with(instance&            target,
                         std::vector<operand> args,
                         const source&        src,
                         bool                 through_member) -> result<operand> {
    TRY(comp_.infer_return(target, u_, src));

    // An `extern` function's body is the linker's, so it only runs at runtime
    if (target.linkage == gir::linkage::EXTERN && is_comptime()) {
        return comp_.needs_runtime(u_, src);
    }

    // A call returning `type` always runs at compile time
    if (is_comptime() || target.comptime_only) {
        auto value{[&] -> result<operand> {
            const auto             scope{comptime_scope()};
            std::vector<ip::index> values;
            values.reserve(args.size());
            for (usize i{0}; i < args.size(); ++i) {
                if (target.params[i]->get_kind() == gs::type_kind::TYPE &&
                    !target.bindings.empty()) {
                    values.emplace_back(*comptime_value(args[i]));
                    continue;
                }

                const auto coerced{TRY(coerce_to(args[i],
                                                 *target.params[i],
                                                 src,
                                                 {.what      = use_site::kind::ARGUMENT,
                                                  .arg_index = i + 1,
                                                  .callee    = target.symbol}))};
                values.emplace_back(TRY(require_comptime(coerced, src)));
            }

            return comp_.call_comptime(u_, src, target, std::move(values));
        }()};
        if (is_comptime()) { return value; }
        return at_comptime_boundary(std::move(value));
    }

    std::vector<gir::value> values;
    values.reserve(args.size());
    for (usize i{0}; i < args.size(); ++i) {
        auto& param{*target.params[i]};

        // A type parameter's slot is left undefined, since the instance already knows it
        if (param.get_kind() == gs::type_kind::TYPE && !target.bindings.empty()) {
            values.emplace_back(constant(*comptime_value(args[i]), param));
            continue;
        }

        const auto coerced{TRY(coerce_to(
            args[i],
            param,
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

} // namespace ghoti::uir::sema
