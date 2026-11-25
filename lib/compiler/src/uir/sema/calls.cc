#include <algorithm>
#include <functional>
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
    const auto  outer{std::exchange(calling_, inst)};
    const auto  restore{gsl::finally([&] { calling_ = outer; })};
    return with_modifier(modifier, src, [&] -> result<operand> {
        const auto callee_src{as_inst(callee_ref).transform([&](inst_id i) { return src_of(i); })};
        const auto callee{TRY(resolve_value(callee_ref, src))};
        const bool expands{std::ranges::any_of(pack_mask, [](u32 word) { return word != 0; })};

        // `.name(args)` calls the member of whatever type the call is used as
        if (callee.is<enum_literal>() && !expands) { return implicit_call{inst}; }
        return call_expanding(callee, args, pack_mask, src, callee_src.value_or(src));
    });
}

auto analyzer::with_modifier(stdx::option<ref>                       modifier,
                             const source&                           src,
                             const std::function<result<operand>()>& call) -> result<operand> {
    if (!modifier) { return call(); }

    // The modifier is a `builtin.CallModifier` known at compile time
    const auto at{as_inst(*modifier).transform([&](inst_id i) { return src_of(i); }).value_or(src)};
    const auto value{[&] {
        const auto scope{comptime_scope()};
        return resolve_value(*modifier, at);
    }()};
    std::string name;
    if (value) {
        if (const auto literal{value->as_opt<enum_literal>()}) {
            name = literal_name(*literal);
        } else if (const auto index{comptime_value(*value)}) {
            if (const auto e{comp_.constants().value_at(*index).data.as_opt<ip::enum_value>()}) {
                name = e->name;
            }
        }
    }

    const auto how{ast::call_modifier_from_name(name)};
    if (!how) {
        return comp_.report(u_,
                            at,
                            gs::error::COMPTIME_EVALUATION_FAILED,
                            "'@call' modifier must be known at compile time");
    }

    if (*how == ast::call_modifier::AUTO) { return call(); }
    if (*how == ast::call_modifier::COMPILE_TIME) {
        auto folded{[&] {
            const auto scope{comptime_scope()};
            return call();
        }()};
        if (!folded && folded.error() == stop::RUNTIME) {
            return comp_.report_runtime_need(
                u_,
                stdx::none,
                gs::error::COMPTIME_EVALUATION_FAILED,
                "'@call(.compile_time, ...)': this call can't be evaluated at compile time");
        }

        return folded;
    }

    auto result{TRY(call())};
    if (!is_comptime()) { builder_.mark_last_call(*how); }
    return result;
}

auto analyzer::call_expanding(const operand&       callee,
                              gsl::span<const ref> args,
                              gsl::span<const u32> pack_mask,
                              const source&        src,
                              const source&        callee_src) -> result<operand> {
    const bool expands{std::ranges::any_of(pack_mask, [](u32 word) { return word != 0; })};
    if (!expands) { return call_operand(callee, args, src, callee_src); }

    // `f(rest...)` passes each of the pack's values as an argument of its own
    const auto generic{callee.as_opt<generic_ref>()};
    const auto function{callee.as_opt<function_ref>()};
    if (!generic && !function) {
        return comp_.unsupported(u_, src, "a pack expansion into this callee");
    }

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

    if (generic) { return call_generic(*generic->fn, std::move(values), src, callee_src); }

    // A plain function's fixed parameters take the expanded values in order
    auto& target{*function->fn};
    if (values.size() != target.params.size() ||
        target.type->get_data().as<gs::types::function>().is_variadic) {
        return comp_.report(
            u_,
            callee_src,
            gs::error::ARITY_MISMATCH,
            fmt::format("Expected {} arguments, found {}", target.params.size(), values.size()));
    }

    std::vector<operand> operands;
    for (auto& value : values) { operands.emplace_back(std::move(value.value)); }
    return call_following_poly(target, std::move(operands), src, false);
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

    if (const auto body{comp_.closure_body(type_of(callee))}) {
        return call_closure(callee, *body, args, src);
    }

    // A compile-time function value calls its own copy directly
    if (const auto index{comptime_value(callee)}) {
        if (const auto target{comp_.const_function(*index)}) {
            return call_function(*target, args, src, callee_src, false);
        }
    }

    // A capturing local generic is called through the environment its binding holds
    if (const auto g{comp_.closure_generic(type_of(callee))}) {
        std::vector<call_arg> values;
        for (const auto arg : args) {
            values.emplace_back(call_arg{.value = TRY(resolve_value(arg, src)), .written = arg});
        }

        // A generic that captures nothing is its instances, called directly
        if (!g->env) { return call_generic(*g, std::move(values), src, callee_src); }
        TRY(require_runtime(src));
        const auto env{TRY(closure_address(callee, src))};
        return call_generic(*g, std::move(values), src, callee_src, env);
    }

    const auto fn{callee.as_opt<function_ref>()};
    if (!fn) {
        // A function value or `^fn(...)` is called through
        auto&      callee_type{type_of(callee)};
        const auto pointee{callee_type.get_data().as_opt<gs::types::pointer>()};
        const auto fn_type{
            (pointee ? pointee->underlying : callee_type).get_data().as_opt<gs::types::function>()};
        if (!fn_type || is_comptime()) {
            return comp_.unsupported(u_, src, "a call through a value");
        }

        return call_value(TRY(as_runtime(callee, src)), *fn_type, args, src);
    }

    if (fn->fn->env) { return call_thunk(*fn->fn, args, src); }
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
    const auto outer{std::exchange(calling_, inst)};
    const auto restore{gsl::finally([&] { calling_ = outer; })};
    return with_modifier(modifier, src, [&] {
        return field_call_unmodified(inst, object_ref, name, args, pack_mask);
    });
}

auto analyzer::field_call_unmodified(inst_id              inst,
                                     ref                  object_ref,
                                     std::string_view     name,
                                     gsl::span<const ref> args,
                                     gsl::span<const u32> pack_mask) -> result<operand> {
    const auto& src{src_of(inst)};
    const auto  object{TRY(resolve(object_ref, src))};

    // A module's function is called like one declared here
    if (const auto m{object.as_opt<module_ref>()}) {
        const auto member{TRY(module_member(*m, name, src))};
        return call_expanding(member, args, pack_mask, src, src);
    }

    for (const auto word : pack_mask) {
        if (word != 0) { return comp_.unsupported(u_, src, "a pack expansion into a method"); }
    }

    const auto index{comptime_value(object)};
    if (!index || !comp_.constants().is_type(*index)) {
        if (const auto through_vtable{TRY(call_dyn(inst, object, name, args, src))}) {
            return *through_vtable;
        }

        const auto owner{receiver_type(object)};
        if (!owner) { return comp_.unsupported(u_, src, "a call through a field"); }

        // `s.f()` of a field holding a function calls through the function value
        const auto st{owner->get_data().as_opt<gs::types::struct_t>()};
        const auto un{owner->get_data().as_opt<gs::types::union_t>()};
        if ((st && std::ranges::contains(st->field_names, name)) ||
            (un && std::ranges::contains(un->field_names, name))) {
            return call_through_field(object_ref, name, args, src);
        }

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

auto analyzer::call_through_field(ref                  object_ref,
                                  std::string_view     name,
                                  gsl::span<const ref> args,
                                  const source&        src) -> result<operand> {
    TRY(require_runtime(src));
    const auto base{TRY(address_of(object_ref, src))};
    const auto field{TRY(field_place(base, name, src))};
    auto&      field_type{*field.pointee};
    const auto pointer{field_type.get_data().as_opt<gs::types::pointer>()};
    const auto fn{
        (pointer ? pointer->underlying : field_type).get_data().as_opt<gs::types::function>()};
    if (!fn) { return comp_.unsupported(u_, src, "a call through a field"); }
    return call_value(TRY(read(field, src)), *fn, args, src);
}

auto analyzer::call_value(const gir::value&          callee,
                          const gs::types::function& fn,
                          gsl::span<const ref>       args,
                          const source&              src) -> result<operand> {
    if (args.size() != fn.params.size()) {
        return comp_.report(
            u_,
            src,
            gs::error::ARITY_MISMATCH,
            fmt::format("Expected {} arguments, found {}", fn.params.size(), args.size()));
    }

    std::vector<gir::value> values;
    values.reserve(args.size());
    for (usize i{0}; i < args.size(); ++i) {
        const auto given{TRY(resolve_value(args[i], src))};
        const auto coerced{TRY(coerce_to(
            given, *fn.params[i], src, {.what = use_site::kind::ARGUMENT, .arg_index = i + 1}))};
        values.emplace_back(TRY(as_runtime(coerced, src)));
    }

    auto&      ret{fn.return_type};
    const auto result{builder_.emit_indirect_call(callee, std::move(values), ret)};
    if (!result) { return void_value(); }
    return gir::value{*result, ret};
}

auto analyzer::call_function(instance&            target,
                             gsl::span<const ref> args,
                             const source&        src,
                             const source&        callee_src,
                             bool                 through_member) -> result<operand> {
    // A call leaving trailing defaulted arguments off goes to the declaration's wrapper for them
    const bool variadic{target.type->get_data().as<gs::types::function>().is_variadic};
    if (!variadic && args.size() < target.params.size()) {
        const auto written{args.size() - (target.self_modifier && !args.empty() ? 1 : 0)};
        if (const auto wrapper{TRY(default_wrapper(*target.owner, target.decl, written, src))}) {
            if (const auto fn{wrapper->as_opt<function_ref>()}) {
                return call_function(*fn->fn, args, src, callee_src, through_member);
            }

            return call_operand(*wrapper, args, src, callee_src);
        }
    }

    // A C-variadic function takes any arguments after its parameters
    if (variadic && args.size() < target.params.size()) {
        return comp_.report(u_,
                            callee_src,
                            gs::error::ARITY_MISMATCH,
                            fmt::format("Expected at least {} arguments, found {}",
                                        target.params.size(),
                                        args.size()));
    }

    if (!variadic && args.size() != target.params.size()) {
        return comp_.report(
            u_,
            callee_src,
            gs::error::ARITY_MISMATCH,
            fmt::format("Expected {} arguments, found {}", target.params.size(), args.size()));
    }

    std::vector<operand> values;
    values.reserve(args.size());
    for (usize i{0}; i < args.size(); ++i) {
        // An array in comptime memory passed for a slice is viewed where it lies
        if (is_comptime() && i < target.params.size() &&
            target.params[i]->get_kind() == gs::type_kind::SLICE) {
            const auto given{TRY(resolve(args[i], src))};
            if (const auto view{given.is<comptime_place>() ? view_of(given) : stdx::none}) {
                values.emplace_back(memory_slice(*view, 0, view->len));
                continue;
            }
        }

        values.emplace_back(TRY(resolve_value(args[i], src)));
    }

    return call_following_poly(target, std::move(values), src, through_member);
}

auto analyzer::call_with(instance&            target,
                         std::vector<operand> args,
                         const source&        src,
                         bool                 through_member) -> result<operand> {
    const auto env{std::exchange(call_env_, stdx::none)};
    if (calling_) { callees_.insert_or_assign(*calling_, &target); }
    if (target.env && is_comptime()) { return comp_.needs_runtime(u_, src); }
    TRY(comp_.infer_return(target, u_, src));
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
        // A variadic argument is passed in its own type
        if (i >= target.params.size()) {
            // An untyped number goes as itself, which codegen promotes like C
            const auto known{comptime_value(args[i])};
            if (known && gs::is_comptime_numeric(type_of(args[i]).get_kind())) {
                values.emplace_back(constant(*known, type_of(args[i])));
                continue;
            }

            const auto extra{TRY(concrete(args[i], src))};
            values.emplace_back(TRY(as_runtime(extra, src)));
            continue;
        }

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
    if (!target.comptime_env.empty()) {
        return comp_.unsupported(u_, src, "a compile-time closure called at runtime");
    }

    comp_.request_body(target);

    auto& ret{*target.return_type};

    // A local generic's instance takes the environment first
    if (target.env && env) {
        auto first{*env};
        first.type.emplace(*target.type->get_data().as<gs::types::function>().params[0]);
        values.insert(values.begin(), first);
    }

    // Call a type's member function through its name as a constant
    const auto result{
        through_member
            ? builder_.emit_indirect_call(
                  constant(comp_.constants().intern(target.symbol, *target.type), *target.type),
                  std::move(values),
                  ret)
            : builder_.emit_call(target.symbol, std::move(values), ret)};
    if (ret.get_kind() == gs::type_kind::NORETURN) {
        builder_.emit_unreachable();
        return comp_.constants().intern(ip::undefined_value{}, ret);
    }
    if (!result) { return void_value(); }
    return gir::value{*result, ret};
}

} // namespace ghoti::uir::sema
