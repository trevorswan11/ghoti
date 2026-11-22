#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/id.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/pattern.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"

namespace ghoti::uir::sema {

namespace {

[[nodiscard]] auto unref(gs::type& t) -> gs::type& {
    if (t.get_kind() == gs::type_kind::REFERENCE) { return *gs::pointee_of(t); }
    return t;
}

} // namespace

auto analyzer::prelude_impl(gs::type& target, std::string_view name, const source& src)
    -> result<stdx::option<impl_instance&>> {
    const auto iface{TRY(comp_.prelude_type(name, u_, src))};
    for (const auto applied : TRY(comp_.impls_of(target))) {
        if (applied->interface && applied->interface.get() == iface.get()) {
            return stdx::option<impl_instance&>{*applied};
        }
    }

    return stdx::option<impl_instance&>{};
}

auto analyzer::associated_type(impl_instance& applied, std::string_view name, const source& src)
    -> result<stdx::option<gs::type&>> {
    const auto member{TRY(comp_.impl_member(applied, name, src))};
    if (!member) { return stdx::option<gs::type&>{}; }
    const auto index{member->as_opt<ip::index>()};
    if (!index || !comp_.constants().is_type(*index)) { return stdx::option<gs::type&>{}; }
    return stdx::option<gs::type&>{comp_.constants().type_at(*index)};
}

auto analyzer::call_impl_method(impl_instance&       applied,
                                std::string_view     name,
                                std::vector<operand> args,
                                const source&        src) -> result<operand> {
    const auto member{TRY(comp_.impl_member(applied, name, src))};
    if (member) {
        if (const auto fn{member->as_opt<function_ref>()}) {
            return call_with(*fn->fn, std::move(args), src, false);
        }

        if (const auto generic{member->as_opt<generic_ref>()}) {
            std::vector<call_arg> values;
            values.reserve(args.size());
            for (auto& arg : args) { values.emplace_back(call_arg{.value = std::move(arg)}); }
            return call_generic(*generic->fn, std::move(values), src, src);
        }
    }

    return comp_.unsupported(u_, src, fmt::format("an impl's `{}` that isn't a function", name));
}

auto analyzer::unwrap_shape_of(gs::type& operand_type, const source& src)
    -> result<stdx::option<unwrap_shape>> {
    auto&      target{unref(operand_type)};
    const auto applied{TRY(prelude_impl(target, "Unwrappable", src))};
    if (!applied) { return stdx::option<unwrap_shape>{}; }

    const auto output{TRY(associated_type(*applied, "Output", src))};
    const auto residual{TRY(associated_type(*applied, "Residual", src))};
    if (!output || !residual) { return stdx::option<unwrap_shape>{}; }
    return stdx::option<unwrap_shape>{
        unwrap_shape{.applied = &*applied, .output = &*output, .residual = &*residual}};
}

auto analyzer::emit_flow(ref                 operand_ref,
                         const operand&      op,
                         const unwrap_shape& shape,
                         const source&       src) -> result<flow_result> {
    auto& ctx{comp_.ctx()};
    auto& operand_type{type_of(op)};
    auto& target{unref(operand_type)};

    // `branch` only reads its operand, so a `mut?` reference to it is enough
    gir::value address;
    bool       is_const{true};
    bool       is_poly{false};
    if (operand_type.get_kind() == gs::type_kind::REFERENCE) {
        address  = TRY(as_runtime(op, src));
        is_const = operand_type.is_constant();
        is_poly  = operand_type.is_poly();
    } else {
        const auto storage{TRY(address_of(operand_ref, src))};
        address  = storage.address;
        is_const = !storage.is_mutable;
    }

    auto&                self_ref{ctx.get_reference(gs::types::mut::POLY, target)};
    std::vector<operand> args;
    args.emplace_back(gir::value{address.data, self_ref});
    const auto branched{TRY(call_impl_method(*shape.applied, "branch", std::move(args), src))};
    const auto flow{TRY(as_runtime(branched, src))};
    auto&      flow_type{*flow.type};
    const auto slot{spill(flow, flow_type, false).address};

    auto&      i32_type{ctx.get_int(32, true)};
    const auto tag_at{
        builder_.emit_get_element_ptr(slot, {usize_constant(UNION_TAG_FIELD)}, i32_type)};
    const gir::value tag_value{builder_.emit_load(gir::value{tag_at, i32_type}, i32_type),
                               i32_type};

    const auto break_index{TRY(field_index(flow_type, "break", src))};
    auto&      bool_type{builtin(gs::type_kind::BOOL)};
    const auto is_break{builder_.emit_binary(
        gir::instruction_kind::EQ,
        tag_value,
        constant(comp_.constants().intern(static_cast<i64>(break_index), i32_type), i32_type),
        bool_type)};
    return flow_result{
        .slot     = slot,
        .is_break = gir::value{is_break, bool_type},
        .is_const = is_const,
        .is_poly  = is_poly,
    };
}

auto analyzer::flow_output(const flow_result& flow, const unwrap_shape& shape) -> gir::value {
    auto&      out_ref{comp_.ctx().get_reference(gs::types::mut::POLY, *shape.output)};
    const auto payload_at{
        builder_.emit_get_element_ptr(flow.slot, {usize_constant(UNION_PAYLOAD_FIELD)}, out_ref)};
    const auto out_address{builder_.emit_load(gir::value{payload_at, out_ref}, out_ref)};
    return gir::value{builder_.emit_load(gir::value{out_address, out_ref}, *shape.output),
                      *shape.output};
}

auto analyzer::propagation_for(const operand& op, const unwrap_shape& shape, const source& src)
    -> result<propagation> {
    auto& ctx{comp_.ctx()};
    if (!fn_) {
        return comp_.report(u_,
                            src,
                            gs::error::UNWRAP_OUTSIDE_FUNCTION,
                            "the '?' operator can only be used inside a function");
    }

    if (fn_->infers_return) {
        return comp_.report(u_,
                            src,
                            gs::error::UNWRAP_RETURN_TYPE_MISMATCH,
                            "the '?' operator requires the enclosing function's return type to "
                            "be written explicitly rather than inferred with 'auto'");
    }

    auto&      operand_type{type_of(op)};
    auto&      ret{unref(*fn_->return_type)};
    const auto rewrap{TRY(prelude_impl(ret, "Rewrappable", src))};
    if (!rewrap) {
        return comp_.report(
            u_,
            src,
            gs::error::UNWRAP_RETURN_TYPE_MISMATCH,
            fmt::format("the '?' operator propagates a '{}' residual ('{}') but '{}' does not "
                        "implement 'builtin.Rewrappable'",
                        ctx.type_display_name(operand_type),
                        ctx.type_display_name(*shape.residual),
                        ctx.type_display_name(*fn_->return_type)));
    }

    // `from_residual`'s parameter names what it rebuilds from, else the impl's `From`
    stdx::option<gs::type&> from;
    if (const auto member{TRY(comp_.impl_member(*rewrap, "from_residual", src))}) {
        if (const auto fn{member->as_opt<function_ref>()}; fn && !fn->fn->params.empty()) {
            from.emplace(*fn->fn->params.front());
        }
    }

    if (!from) { from = TRY(associated_type(*rewrap, "From", src)); }
    if (!from) { return comp_.unsupported(u_, src, "a `Rewrappable` impl with no `From`"); }

    auto& residual{*shape.residual};
    if (!gs::is_same_unqualified(residual, *from) && !gs::is_assignable(residual, *from) &&
        !gs::is_implicit_widenable(residual, *from)) {
        std::string reason;
        if (const auto why{gs::cast_rejection_reason(
                residual, *from, comp_.ptr_bits(), ctx.user_type_names)}) {
            reason = fmt::format(" ({})", *why);
        }

        return comp_.report(
            u_,
            src,
            gs::error::UNWRAP_RETURN_TYPE_MISMATCH,
            fmt::format("the '?' operator propagates a '{}' residual ('{}') but '{}' is not "
                        "rebuildable from '{}'{}; implement 'builtin.Rewrappable for {}' with "
                        "From = '{}'",
                        ctx.type_display_name(operand_type),
                        ctx.type_display_name(residual),
                        ctx.type_display_name(*fn_->return_type),
                        ctx.type_display_name(residual),
                        reason,
                        ctx.type_display_name(*fn_->return_type),
                        ctx.type_display_name(residual)));
    }

    return propagation{
        .flow = {}, .shape = shape, .rewrap = &*rewrap, .from = &*from, .rewrapped = {}};
}

auto analyzer::analyze_unwrap(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  operand_ref{*rec.ref_field()};
    const auto  kind{static_cast<unwrap_kind>(rec.u32_field())};
    const auto  failure{rec.body_field()};
    const bool  propagates{kind == unwrap_kind::PROPAGATE};

    const auto op{TRY(resolve_value(operand_ref, src))};
    auto&      operand_type{type_of(op)};
    if (operand_type.is_poison()) { return stdx::err{stop::POISON}; }

    auto&      ctx{comp_.ctx()};
    const auto shape{TRY(unwrap_shape_of(operand_type, src))};
    if (!shape) {
        // A two-field tagged union is likely meant to unwrap
        std::string note;
        if (const auto un{unref(operand_type).get_data().as_opt<gs::types::union_t>()};
            un && !un->is_untagged && un->fields.size() == 2) {
            note = fmt::format("; add 'impl builtin.Unwrappable for {}'",
                               ctx.type_display_name(operand_type));
        }

        return comp_.report(u_,
                            src,
                            gs::error::UNWRAP_ON_NON_RESULT,
                            fmt::format("the postfix '{}' operator expects a type implementing "
                                        "'builtin.Unwrappable'; '{}' does not implement it{}",
                                        propagates ? "?" : "!",
                                        ctx.type_display_name(operand_type),
                                        note));
    }

    stdx::option<propagation> propagated;
    if (propagates) { propagated.emplace(TRY(propagation_for(op, *shape, src))); }
    if (is_comptime()) { return comp_.unsupported(u_, src, "an unwrap at compile time"); }

    const auto flow{TRY(emit_flow(operand_ref, op, *shape, src))};
    const auto payload_seg{new_segment()};
    const auto diverge_seg{new_segment()};
    builder_.emit_cond_goto(flow.is_break, diverge_seg, payload_seg);

    switch_to(diverge_seg);
    if (propagated) {
        propagated->flow = flow;
        propagations_.emplace_back(std::move(*propagated));
        const auto out{analyze_body(failure)};
        propagations_.pop_back();
        DISCARD(TRY(out));
    } else {
        TRY(emit_panic(shape->residual->get_kind() == gs::type_kind::VOID_
                           ? "'!' unwrapped an empty optional"
                           : "'!' unwrapped an errored result",
                       src));
    }

    switch_to(payload_seg);
    return flow_output(flow, *shape);
}

auto analyzer::bind_unwrapped(gsl::span<const inst_id> body, u32 slot, const operand& value)
    -> void {
    for (const auto i : body) {
        if (mod().tag_of(i) != tag::bind) { continue; }
        record_reader b{record(i)};
        b.skip_str();
        if (b.u32_field() == slot) { values_.insert_or_assign(i, value); }
    }
}

auto analyzer::payload_capture(const ast::capture_t& capture,
                               const flow_result&    flow,
                               const unwrap_shape&   shape) -> result<operand> {
    auto&                           output{*shape.output};
    const gs::pattern::capture_site site{
        .capture = capture, .value_type = output, .place_is_const = flow.is_const};
    const match_root root{
        .address = {}, .is_const = flow.is_const, .is_addressable = true, .is_poly = flow.is_poly};
    auto& cap_type{*TRY(capture_type(site, root))};

    // The `continue` payload is a reference to the output inside the operand
    auto&      out_ref{comp_.ctx().get_reference(gs::types::mut::POLY, output)};
    const auto payload_at{
        builder_.emit_get_element_ptr(flow.slot, {usize_constant(UNION_PAYLOAD_FIELD)}, out_ref)};
    const auto       out_address{builder_.emit_load(gir::value{payload_at, out_ref}, out_ref)};
    const gir::value slot{builder_.emit_alloca(cap_type), cap_type};
    if (capture.modifier.is_value()) {
        const auto copied{builder_.emit_load(gir::value{out_address, out_ref}, output)};
        builder_.emit_store(slot, gir::value{copied, output}).is_initializer = true;
    } else {
        builder_.emit_store(slot, gir::value{out_address, cap_type}).is_initializer = true;
    }

    return operand{slot_value{.address = slot, .type = &cap_type}};
}

auto analyzer::residual_capture(const flow_result& flow, const unwrap_shape& shape) -> operand {
    auto&      residual{*shape.residual};
    const auto residual_at{
        builder_.emit_get_element_ptr(flow.slot, {usize_constant(UNION_PAYLOAD_FIELD)}, residual)};
    const auto       loaded{builder_.emit_load(gir::value{residual_at, residual}, residual)};
    const gir::value slot{builder_.emit_alloca(residual), residual};
    builder_.emit_store(slot, gir::value{loaded, residual}).is_initializer = true;
    return operand{slot_value{.address = slot, .type = &residual}};
}

auto analyzer::capture_shape(const operand&                      op,
                             const stdx::option<ast::capture_t>& residual,
                             std::string_view                    construct,
                             const source&                       cond_src,
                             const source&                       src) -> result<unwrap_shape> {
    auto& operand_type{type_of(op)};
    if (operand_type.is_poison()) { return stdx::err{stop::POISON}; }

    auto&      ctx{comp_.ctx()};
    const auto shape{TRY(unwrap_shape_of(operand_type, src))};
    if (!shape) {
        return comp_.report(u_,
                            cond_src,
                            gs::error::ILLEGAL_UNWRAP_CAPTURE,
                            fmt::format("A capture on `{}` needs a condition that implements "
                                        "'builtin.Unwrappable'; '{}' does not",
                                        construct,
                                        ctx.type_display_name(operand_type)));
    }

    if (!residual) { return *shape; }
    const source residual_src{ast::node_id{*residual->payload}};
    if (shape->residual->get_kind() == gs::type_kind::VOID_) {
        return comp_.report(u_,
                            residual_src,
                            gs::error::ILLEGAL_UNWRAP_CAPTURE,
                            fmt::format("'{}' carries nothing when it doesn't unwrap, so its "
                                        "`else` has no value to capture",
                                        ctx.type_display_name(operand_type)));
    }

    if (!residual->modifier.is_value()) {
        return comp_.report(u_,
                            residual_src,
                            gs::error::ILLEGAL_UNWRAP_CAPTURE,
                            "An `else` capture is the residual itself, so it takes no `&` or `^` "
                            "modifier");
    }

    return *shape;
}

auto analyzer::analyze_unwrap_branch(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  cond_ref{*rec.ref_field()};
    const auto  then_body{rec.body_field()};
    const auto  else_body{rec.body_field()};
    const auto& node{u_.source->ast.get_as<ast::if_expr>(*src.as_opt<ast::node_id>())};
    const auto  cond_src{as_inst(cond_ref).transform([&](inst_id i) { return src_of(i); })};

    const auto op{TRY(resolve_value(cond_ref, src))};
    const auto shape{TRY(capture_shape(op, node.else_capture, "if", cond_src.value_or(src), src))};
    if (is_comptime()) { return comp_.unsupported(u_, src, "an unwrapping `if` at compile time"); }

    const auto flow{TRY(emit_flow(cond_ref, op, shape, src))};
    const auto then_seg{new_segment()};
    const auto else_seg{new_segment()};
    const auto merge{new_segment()};
    builder_.emit_cond_goto(flow.is_break, else_seg, then_seg);

    std::vector<incoming> arrivals;
    const auto            branch{runtime_branch()};
    for (const auto& [seg, body, is_payload] :
         {std::tuple{then_seg, then_body, true}, std::tuple{else_seg, else_body, false}}) {
        switch_to(seg);
        if (is_payload) {
            const auto captured{TRY(payload_capture(*node.payload_capture, flow, shape))};
            bind_unwrapped(body, 0, captured);
        } else if (node.else_capture) {
            bind_unwrapped(body, 1, residual_capture(flow, shape));
        }

        const auto out{TRY(analyze_body(body))};
        if (out.yielded && !terminated()) {
            arrivals.emplace_back<incoming>(
                {.seg = builder_.get_segment()->get_id(), .value = *out.yielded, .src = src});
            emit_goto(merge);
        }
    }

    return finish_merge(arrivals, merge, src);
}

auto analyzer::analyze_residual(inst_id inst) -> result<operand> {
    const auto& src{src_of(inst)};
    if (propagations_.empty()) { return comp_.unsupported(u_, src, "a residual outside a `?`"); }
    auto& p{propagations_.back()};
    auto& residual_type{*p.shape.residual};

    operand residual{void_value()};
    if (residual_type.get_kind() != gs::type_kind::VOID_) {
        const auto payload_at{builder_.emit_get_element_ptr(
            p.flow.slot, {usize_constant(UNION_PAYLOAD_FIELD)}, residual_type)};
        residual =
            gir::value{builder_.emit_load(gir::value{payload_at, residual_type}, residual_type),
                       residual_type};
    }

    auto&      from{*p.from};
    const auto converted{TRY(coerce_to(residual, from, src, {.what = use_site::kind::RETURN}))};

    // An errdefer reads the residual from its own slot
    operand kept{void_value()};
    if (from.get_kind() != gs::type_kind::VOID_) {
        const auto slot{spill(TRY(as_runtime(converted, src)), from, false)};
        kept = slot_value{.address = slot.address, .type = &from};
    }

    std::vector<operand> args;
    args.emplace_back(converted);
    p.rewrapped.emplace(TRY(call_impl_method(*p.rewrap, "from_residual", std::move(args), src)));
    return kept;
}

auto analyzer::analyze_ret_residual(inst_id inst) -> result<operand> {
    const auto& src{src_of(inst)};
    if (propagations_.empty() || !propagations_.back().rewrapped) {
        return comp_.unsupported(u_, src, "a residual return outside a `?`");
    }

    const auto& rewrapped{*propagations_.back().rewrapped};
    auto&       ret{*fn_->return_type};
    if (ret.get_kind() == gs::type_kind::VOID_) {
        builder_.emit_return();
    } else {
        builder_.emit_return(TRY(as_runtime(rewrapped, src)));
    }

    return void_value();
}

} // namespace ghoti::uir::sema
