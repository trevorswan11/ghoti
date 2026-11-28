#include <algorithm>
#include <ranges>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/span>
#include <gsl/util>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/expression.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"

namespace ghoti::uir::sema {

auto analyzer::void_value() -> operand {
    return comp_.constants().intern(ip::void_value{}, builtin(gs::type_kind::VOID_));
}

auto analyzer::condition(const operand& op, const source& src) -> result<gir::value> {
    auto& type{type_of(op)};
    if (type.is_poison()) { return stdx::err{stop::POISON}; }
    // A pointer holds when it isn't null
    if (type.get_kind() == gs::type_kind::POINTER) {
        auto&      bool_type{builtin(gs::type_kind::BOOL)};
        auto&      null_type{builtin(gs::type_kind::NULLPTR)};
        const auto ptr{TRY(as_runtime(op, src))};
        return gir::value{
            builder_.emit_binary(
                gir::instruction_kind::NE,
                ptr,
                constant(comp_.constants().intern(ip::nullptr_value{}, null_type), null_type),
                bool_type),
            bool_type};
    }

    if (type.get_kind() != gs::type_kind::BOOL) {
        // An `Unwrappable` condition is unwrapped, which a capture has to receive
        if (TRY(unwrap_shape_of(type, src))) {
            return comp_.report(u_,
                                src,
                                gs::error::ILLEGAL_UNWRAP_CAPTURE,
                                fmt::format("A condition on a '{}' unwraps it, so it needs a "
                                            "capture; write `|_|` to ignore the payload",
                                            comp_.ctx().type_display_name(type)));
        }

        return comp_.report(
            u_, src, gs::error::TYPE_MISMATCH, messages::condition_not_bool(comp_.ctx(), type));
    }

    return as_runtime(op, src);
}

auto analyzer::unrolls(ref r) const -> bool {
    const auto inst{as_inst(r)};
    return inst && std::ranges::contains(unrolling_, *inst, &unrolled_loop::inst);
}

auto analyzer::target_of(ref r) -> target& {
    const auto inst{*as_inst(r)};
    for (auto& t : targets_ | std::views::reverse) {
        if (t.inst == inst) { return t; }
    }

    UNREACHABLE("AstGen only breaks to an enclosing block or loop");
}

auto analyzer::arrive(target& t, const operand& value, const source& src) -> void {
    if (!t.exit) { t.exit = new_segment(); }

    // A type from an arm a known condition skips never reaches the merge
    const auto known{comptime_value(value)};
    if (untaken_ == 0 || !known || !comp_.constants().is_type(*known)) {
        t.arrivals.emplace_back<incoming>(
            {.seg = builder_.get_segment()->get_id(), .value = value, .src = src});
    }
    emit_goto(*t.exit);
}

auto analyzer::merge_holds_literals(merge_ref m) const -> bool {
    const auto& first{merges_[m.index].arrivals.front().value};
    if (const auto inner{first.as_opt<merge_ref>()}) { return merge_holds_literals(*inner); }
    return is_untyped_literal(first);
}

auto analyzer::finish_merge(const std::vector<incoming>& arrivals,
                            gir::segment_id              merge,
                            const source&                src) -> result<operand> {
    switch_to(merge);
    if (arrivals.empty()) {
        builder_.emit_unreachable();
        return void_value();
    }

    // Types only exist at compile time, so a runtime branch can't choose between them
    if (std::ranges::all_of(arrivals,
                            [&](const incoming& arrival) {
                                const auto known{comptime_value(arrival.value)};
                                return known && comp_.constants().is_type(*known);
                            }) &&
        arrivals.size() > 1) {
        return comp_.report(u_,
                            src,
                            gs::error::COMPTIME_EVALUATION_FAILED,
                            "A type computed by an expression must be known at compile time");
    }

    std::vector<operand> values;
    values.reserve(arrivals.size());
    bool all_void{true};
    bool all_untyped_constants{true};
    bool all_untyped_literals{true};
    for (const auto& arrival : arrivals) {
        values.emplace_back(arrival.value);

        // An inner merge still waiting for its type waits with this one
        if (const auto inner{arrival.value.as_opt<merge_ref>()}) {
            all_void = false;
            if (merge_holds_literals(*inner)) {
                all_untyped_constants = false;
            } else {
                all_untyped_literals = false;
            }
            continue;
        }

        // An untyped literal has no type of its own to check here
        const bool literal{is_untyped_literal(arrival.value)};
        all_untyped_literals = all_untyped_literals && literal;
        if (literal) {
            all_void              = false;
            all_untyped_constants = false;
            continue;
        }

        auto& type{type_of(arrival.value)};
        if (type.is_poison()) { return stdx::err{stop::POISON}; }
        all_void              = all_void && type.get_kind() == gs::type_kind::VOID_;
        all_untyped_constants = all_untyped_constants && comptime_value(arrival.value) &&
                                gs::is_comptime_numeric(type.get_kind());
    }

    if (all_void) { return void_value(); }

    // Every path carrying the same constant makes the merge that constant
    if (const auto first{comptime_value(arrivals.front().value)};
        first && std::ranges::all_of(arrivals, [&](const incoming& a) {
            return comptime_value(a.value) == first;
        })) {
        return arrivals.front().value;
    }

    // The arms take a type where the merge is used
    if (all_untyped_constants || all_untyped_literals) {
        merges_.emplace_back<pending_merge>(
            {.seg = merge, .arrivals = arrivals, .materialized = stdx::none});
        return merge_ref{static_cast<u32>(merges_.size() - 1)};
    }

    // Untyped literals take the type the typed arms agree on
    std::erase_if(values, [&](const operand& v) { return is_untyped_literal(v); });
    auto& peer{*TRY(peer_type(values, src))};

    // A runtime array arm meets a slice of it where it is stored
    const auto sliced{[&](const operand& v) {
        return peer.get_kind() == gs::type_kind::SLICE &&
               type_of(v).get_kind() == gs::type_kind::ARRAY && !comptime_value(v);
    }};

    auto& slot_type{gs::is_comptime_numeric(peer.get_kind()) ? comp_.ctx().default_concrete(peer)
                                                             : peer};
    const auto slot{builder_.emit_alloca_in_entry(slot_type)};
    for (const auto& arrival : arrivals) {
        builder_.set_segment(*gir_fn_->get_segment(arrival.seg));
        builder_.set_splice(true);
        const auto arm{sliced(arrival.value) ? array_slice(arrival, arrival.src)
                                             : result<operand>{arrival.value}};
        const auto stored{
            arm ? coerce_to(*arm, slot_type, arrival.src, {.what = use_site::kind::STORE})
                : result<operand>{stdx::err{arm.error()}}};
        const auto value{stored ? as_runtime(*stored, arrival.src) : stdx::err{stored.error()}};
        if (value) { builder_.emit_store(slot, *value); }
        builder_.set_splice(false);
        if (!value) {
            switch_to(merge);
            return stdx::err{value.error()};
        }
    }

    switch_to(merge);
    return gir::value{builder_.emit_load(slot, slot_type), slot_type};
}

auto analyzer::arm_address(const incoming& arm, const source& src) -> result<gir::value> {
    const auto& array{arm.value};
    auto&       type{type_of(array)};
    if (const auto p{array.as_opt<place>()}) { return settle(*p).address; }
    if (const auto s{array.as_opt<slot_value>()}) { return s->address; }
    if (const auto g{array.as_opt<global_ref>()}) {
        return global_place(g->symbol, *g->type, true).address;
    }

    // A value the arm just loaded is sliced where it was loaded from
    if (const auto v{array.as_opt<gir::value>()}) {
        if (const auto from{unload(*gir_fn_->get_segment(arm.seg), *v, type)}) { return *from; }
    }

    return spill(TRY(as_runtime(array, src)), type, false).address;
}

auto analyzer::unload(gir::segment& seg, const gir::value& v, gs::type& type, bool only_global)
    -> stdx::option<gir::value> {
    auto&      insts{seg.get_instructions()};
    const auto skip{seg.has_terminator() ? usize{2} : usize{1}};
    if (insts.size() < skip) { return stdx::none; }
    const auto last{insts.end() - static_cast<std::ptrdiff_t>(skip)};
    if ((*last)->kind != gir::instruction_kind::LOAD || (*last)->result != v.data ||
        (*last)->operands.empty() || !(*last)->operands.front().type) {
        return stdx::none;
    }

    // A global's address comes right before the load that reads it
    if (only_global) {
        if (last == insts.begin()) { return stdx::none; }
        const auto& address{**std::prev(last)};
        if (address.kind != gir::instruction_kind::GLOBAL_ADDR ||
            address.result != (*last)->operands.front().data) {
            return stdx::none;
        }
    }

    const auto from{(*last)->operands.front().data};
    insts.erase(last);
    return gir::value{from, type};
}

auto analyzer::array_slice(const incoming& arm, const source& src) -> result<operand> {
    const auto address{TRY(arm_address(arm, src))};
    return operand{TRY(slice_of_array(address, type_of(arm.value)))};
}

auto analyzer::analyze_block(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    rec.skip_str();
    const auto body{rec.body_field()};
    if (is_comptime()) { return run_block(inst, body); }

    targets_.emplace_back<target>(
        {.inst = inst, .exit = stdx::none, .repeat = stdx::none, .arrivals = {}});
    const auto out{analyze_body(body)};
    auto       t{std::move(targets_.back())};
    targets_.pop_back();
    const auto flowed{TRY(out)};
    if (!t.exit) { return flowed.yielded.value_or(void_value()); }
    if (flowed.yielded && !terminated()) { arrive(t, *flowed.yielded, src); }
    return finish_merge(t.arrivals, *t.exit, src);
}

auto analyzer::analyze_condbr(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  cond_ref{*rec.ref_field()};
    const auto  then_body{rec.body_field()};
    const auto  else_body{rec.body_field()};
    const auto  cond_src{as_inst(cond_ref).transform([&](inst_id i) { return src_of(i); })};
    const auto  cond{TRY(resolve_value(cond_ref, src))};

    if (is_comptime()) {
        const bool taken{TRY(known_bool(cond, cond_src.value_or(src)))};
        const auto out{TRY(analyze_body(taken ? then_body : else_body))};
        if (jump_) { return void_value(); }
        return out.yielded.value_or(void_value());
    }

    // Both arms are analyzed even when the condition is known when not comptime
    auto       c{TRY(condition(cond, cond_src.value_or(src)))};
    const auto then_seg{new_segment()};
    const auto else_seg{new_segment()};
    const auto merge{new_segment()};

    // With a known `bool` condition only the taken arm reaches the merge
    stdx::option<bool> known;
    if (const auto index{comptime_value(cond)}) {
        if (const auto b{comp_.constants().value_at(*index).data.as_opt<bool>()}) { known = *b; }
    }

    // A statement `if` whose condition folds through immutable bindings takes one arm
    if (!known && yields_nothing(then_body) && yields_nothing(else_body)) {
        known = TRY(folded_condition(cond_ref, cond_src.value_or(src)));
        if (known) { c = constant(comp_.constants().intern(*known, type_of(cond)), type_of(cond)); }
    }

    apply_branch_hints(builder_.emit_cond_goto(c, then_seg, else_seg), then_body, else_body);
    std::vector<incoming> arrivals;
    const auto            branch{runtime_branch()};
    for (const auto& [seg, body, taken] :
         {std::tuple{then_seg, then_body, known.value_or(true)},
          std::tuple{else_seg, else_body, !known.value_or(false)}}) {
        // The arm a known condition skips keeps its type errors, but no value it would compute
        switch_to(seg);
        const auto reported{comp_.diagnostic_count()};
        const bool skipped{known && !taken};
        untaken_ += skipped ? 1 : 0;
        const auto analyzed{analyze_body(body)};
        untaken_ -= skipped ? 1 : 0;
        if (known && !taken && comp_.drop_value_errors(reported)) {
            // An arm cut short by a dropped error leaves the segments it opened unreachable
            const auto here{builder_.get_segment()->get_id()};
            for (auto at{std::to_underlying(merge) + 1};
                 gir_fn_->get_segment_opt(gir::segment_id{at});
                 ++at) {
                if (gir::segment_id{at} == here) { continue; }
                switch_to(gir::segment_id{at});
                if (!terminated()) { builder_.emit_unreachable(); }
            }

            switch_to(here);
            if (!terminated()) { builder_.emit_unreachable(); }
            continue;
        }

        const auto out{TRY(analyzed)};
        if (out.yielded && !terminated()) {
            if (taken) {
                arrivals.emplace_back<incoming>(
                    {.seg = builder_.get_segment()->get_id(), .value = *out.yielded, .src = src});
            }

            emit_goto(merge);
        }
    }

    return finish_merge(arrivals, merge, src);
}

auto analyzer::analyze_loop(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    rec.skip_str();
    const auto loop_flags{rec.u32_field()};
    const auto cond_body{rec.body_field()};
    const auto body{rec.body_field()};
    const auto cont_body{rec.body_field()};
    const auto else_body{rec.body_field()};
    // An unrolled loop runs its control at compile time and copies its body per iteration
    if ((loop_flags & flags::UNROLL) != 0) {
        if (is_comptime()) { return run_loop(inst); }
        const auto unrolled{unrolling(inst)};
        return run_loop(inst);
    }

    // `while (x) |v|` runs `x.branch()` on every iteration
    const bool unwraps{(loop_flags & flags::UNWRAPS) != 0};
    if (is_comptime()) { return run_loop(inst); }
    const auto unwrapping{
        unwraps
            ? stdx::option<const ast::while_loop_expr&>{u_.source->ast.get_as<ast::while_loop_expr>(
                  *src.as_opt<ast::node_id>())}
            : stdx::none};
    stdx::option<flow_result>  flow;
    stdx::option<unwrap_shape> shape;
    const auto                 branch{runtime_branch()};

    const bool has_cond{(loop_flags & flags::HAS_COND) != 0};
    const bool do_while{(loop_flags & flags::DO_WHILE) != 0};
    const auto cond_seg{has_cond ? stdx::option<gir::segment_id>{new_segment()} : stdx::none};
    const auto body_seg{new_segment()};
    const auto cont_seg{cont_body.empty() ? stdx::none
                                          : stdx::option<gir::segment_id>{new_segment()}};
    const auto else_seg{else_body.empty() ? stdx::none
                                          : stdx::option<gir::segment_id>{new_segment()}};
    const auto exit{new_segment()};
    const auto repeat{cont_seg.value_or(cond_seg.value_or(body_seg))};
    targets_.emplace_back<target>({.inst = inst, .exit = exit, .repeat = repeat, .arrivals = {}});
    const auto pop{gsl::finally([this] { targets_.pop_back(); })};

    // The segment the condition's false edge leaves from, for the loop's normal exit
    stdx::option<gir::segment_id> normal_exit;
    const auto                    analyze_cond{[&] -> result<void> {
        switch_to(*cond_seg);
        const auto out{TRY(analyze_body(cond_body))};
        if (!out.yielded || terminated()) {
            return comp_.unsupported(u_, src, "a loop condition that leaves");
        }

        if (unwrapping) {
            const auto cond_ref{mod().data_of(cond_body.back()).un.operand};
            const auto cond_src{as_inst(cond_ref).transform([&](inst_id i) { return src_of(i); })};
            shape.emplace(TRY(capture_shape(
                *out.yielded, unwrapping->else_capture, "while", cond_src.value_or(src), src)));
            flow.emplace(TRY(emit_flow(cond_ref, *out.yielded, *shape, src)));
            normal_exit = builder_.get_segment()->get_id();
            builder_.emit_cond_goto(flow->is_break, else_seg.value_or(exit), body_seg);
            return {};
        }

        const auto c{TRY(condition(*out.yielded, src))};
        normal_exit = builder_.get_segment()->get_id();

        // A condition known to hold never exits, so only a `break` or `return` leaves the loop
        if (const auto known{c.literal_opt<bool>(comp_.constants())}; known && *known) {
            emit_goto(body_seg);
            return {};
        }

        builder_.emit_cond_goto(c, body_seg, else_seg.value_or(exit));
        return {};
    }};

    emit_goto(has_cond && !do_while ? *cond_seg : body_seg);
    if (has_cond && !do_while) { TRY(analyze_cond()); }

    // The payload is captured again where the continuation reads it
    const auto capture_payload{[&] -> result<void> {
        if (!flow) { return {}; }
        const auto captured{TRY(payload_capture(*unwrapping->payload_capture, *flow, *shape))};
        bind_unwrapped(body, 0, captured);
        return {};
    }};

    switch_to(body_seg);
    TRY(capture_payload());
    DISCARD(TRY(analyze_body(body)));
    if (!terminated()) { emit_goto(repeat); }
    if (cont_seg) {
        switch_to(*cont_seg);
        TRY(capture_payload());
        DISCARD(TRY(analyze_body(cont_body)));
        if (!terminated()) { emit_goto(cond_seg.value_or(body_seg)); }
    }

    if (has_cond && do_while) { TRY(analyze_cond()); }

    stdx::option<incoming> normal;
    if (else_seg) {
        switch_to(*else_seg);
        if (flow && unwrapping->else_capture) {
            bind_unwrapped(else_body, 1, residual_capture(*flow, *shape));
        }

        const auto out{TRY(analyze_body(else_body))};
        if (out.yielded && !terminated()) {
            normal.emplace(builder_.get_segment()->get_id(), *out.yielded, src);
            emit_goto(exit);
        }
    } else if (normal_exit) {
        normal.emplace(*normal_exit, void_value(), src);
    }

    auto arrivals{targets_.back().arrivals};
    if (normal) { arrivals.emplace_back<incoming>(std::move(*normal)); }
    return finish_merge(arrivals, exit, src);
}

auto analyzer::unrolled_jump(ref to, bool is_break, const source& src) -> result<void> {
    if (is_comptime() || unrolling_.empty()) { return {}; }
    const auto  name{is_break ? "break" : "continue"};
    const auto  code{is_break ? gs::error::UNROLLED_LOOP_BREAK : gs::error::UNROLLED_LOOP_CONTINUE};
    const auto& inner{unrolling_.back()};
    if (!unrolls(to)) {
        // A target around the unrolled loop has no place in its copied body
        const auto found{std::ranges::find(targets_, *as_inst(to), &target::inst)};
        if (static_cast<usize>(found - targets_.begin()) >= inner.targets) { return {}; }
        return comp_.report(
            u_, src, code, fmt::format("'{}' cannot target a loop outside an unrolled loop", name));
    }

    // Each copy of the body leaves or repeats at compile time, so the jump can't be runtime's
    const auto own{std::ranges::find(unrolling_, *as_inst(to), &unrolled_loop::inst)};
    if (runtime_depth_ <= own->runtime_depth) { return {}; }
    return comp_.report(u_,
                        src,
                        code,
                        fmt::format("'{}' inside an unrolled loop is only allowed within "
                                    "compile-time control flow{}",
                                    name,
                                    is_break ? "" : " (such as `if comptime`)"));
}

auto analyzer::analyze_break(inst_id inst) -> result<operand> {
    const auto& d{mod().data_of(inst)};
    const auto& src{src_of(inst)};
    const auto  value{TRY(resolve_value(d.bin.rhs, src))};
    TRY(unrolled_jump(d.bin.lhs, true, src));
    if (is_comptime() || unrolls(d.bin.lhs)) {
        jump_.emplace(jump::kind::BREAK, *as_inst(d.bin.lhs), value);
        return void_value();
    }

    arrive(target_of(d.bin.lhs), value, src);
    return void_value();
}

auto analyzer::analyze_repeat(inst_id inst) -> result<operand> {
    const auto& operand_ref{mod().data_of(inst).un.operand};
    TRY(unrolled_jump(operand_ref, false, src_of(inst)));
    if (is_comptime() || unrolls(operand_ref)) {
        jump_.emplace(jump::kind::REPEAT, *as_inst(operand_ref), stdx::none);
        return void_value();
    }

    const auto& t{target_of(operand_ref)};
    emit_goto(*t.repeat);
    return void_value();
}

auto analyzer::reject_closure_escape(ref written, const operand& value, const source& src)
    -> result<void> {
    // A capturing generic's value is a function type, its environment the closure behind it
    const auto generic{comp_.closure_generic(type_of(value))};
    const auto env_type{generic && generic->env ? stdx::option<gs::type&>{*generic->env->type}
                                                : stdx::option<gs::type&>{type_of(value)}};
    const auto closure{env_type->get_data().as_opt<gs::types::closure_t>()};
    if (!closure || closure->captures.empty()) { return {}; }

    // A literal returned as written can't hold a reference into the frame that ends
    const auto inst{as_inst(written)};
    const bool literal{inst && mod().tag_of(*inst) == tag::closure};
    const bool borrows{std::ranges::any_of(closure->captures, [](const auto& capture) {
        return capture.mode != gs::types::capture_mode::VALUE;
    })};
    if (literal && borrows) {
        return comp_.report(u_,
                            src,
                            gs::error::ILLEGAL_CLOSURE_ESCAPE,
                            fmt::format("{} captures a reference into its enclosing function and "
                                        "cannot be returned directly; mark it 'move fn' to capture "
                                        "by value instead",
                                        generic ? "Generic function" : "Closure"));
    }

    // An erased callable borrows its closure's environment from this frame
    const auto erased{fn_->return_type->get_data().as_opt<gs::types::function>()};
    if (erased && erased->erased) {
        return comp_.report(u_,
                            src,
                            gs::error::ILLEGAL_CLOSURE_ESCAPE,
                            fmt::format("A capturing {} erased into `fn(...)` borrows its "
                                        "environment from this function and cannot be returned; "
                                        "return it by its own type with an `auto` return instead",
                                        generic ? "generic function" : "closure"));
    }

    return {};
}

auto analyzer::return_outside_function(const source& src) -> result<operand> {
    return comp_.report(
        u_, src, gs::error::ILLEGAL_CONTROL_FLOW, "A return must be inside a function body");
}

auto analyzer::analyze_ret(inst_id inst) -> result<operand> {
    const auto& src{src_of(inst)};
    if (!fn_) { return return_outside_function(src); }
    TRY(reject_unlabeled_loop(mod().data_of(inst).un.operand, src));
    const auto value{TRY(resolve_value(mod().data_of(inst).un.operand, src))};
    TRY(reject_closure_escape(mod().data_of(inst).un.operand, value, src));

    // Collecting the types an inferred return meets records each and leaves
    if (collected_returns_ && fn_->return_type->get_kind() == gs::type_kind::AUTO) {
        auto& type{type_of(value)};
        if (gs::is_numeric(type.get_kind()) || !is_untyped_literal(value)) {
            collected_returns_->emplace_back(&type);
        }

        if (!is_comptime()) { builder_.emit_return(); }
        return void_value();
    }

    // An inferred return type is the first returned value's
    if (fn_->infers_return && fn_->return_type->get_kind() == gs::type_kind::AUTO) {
        // An untyped number only takes its type from the returns after it
        // A cast inferring its target has none to take from an inferred return
        if (const auto cast{value.as_opt<cast_ref>()}) {
            return cast_target_inferred(*cast, stdx::none);
        }

        if (is_untyped_literal(value) ||
            (!is_comptime() && gs::is_comptime_numeric(type_of(value).get_kind()))) {
            return comp_.unsupported(u_, src, "an untyped value as an inferred return");
        }

        fn_->return_type = &inferred_type(value);
    }

    auto& ret{*fn_->return_type};

    // A `mut?` result takes its mutability from a `mut?` parameter, never a view of its own
    const auto view{[](const gs::type& t) {
        return t.get_kind() == gs::type_kind::REFERENCE || t.get_kind() == gs::type_kind::POINTER ||
               t.get_kind() == gs::type_kind::SLICE;
    }};
    if (ret.is_poly() && view(ret) && view(type_of(value)) && !type_of(value).is_poly()) {
        return comp_.report(u_,
                            src,
                            gs::error::RETURN_TYPE_MISMATCH,
                            fmt::format("Return value of type '{}' is not assignable to function "
                                        "return type '{}'",
                                        comp_.ctx().type_display_name(type_of(value)),
                                        comp_.ctx().type_display_name(ret)));
    }

    const auto returned{TRY(coerce_to(value,
                                      ret,
                                      src,
                                      {.what               = use_site::kind::RETURN,
                                       .from               = mod().data_of(inst).un.operand,
                                       .computes_in_target = true}))};
    if (is_comptime()) {
        // A `let mut` returned as an immutable type takes that type, which its readers fold
        auto&      pool{comp_.constants()};
        const auto index{TRY(require_comptime(returned, src))};
        auto&      type{type_of(returned)};
        auto       kept{returned};
        if (&type != &ret && !pool.is_type(index) && gs::is_same_unqualified(type, ret)) {
            kept = operand{pool.intern(ip::value_data{pool.value_at(index).data}, ret)};
        }

        jump_.emplace(jump::kind::RETURN, stdx::none, kept);
        return void_value();
    }

    // An address of a local or parameter dangles once the function returns
    if ((ret.get_kind() == gs::type_kind::POINTER || ret.get_kind() == gs::type_kind::REFERENCE) &&
        addresses_frame(mod().data_of(inst).un.operand)) {
        return comp_.report(u_,
                            src,
                            gs::error::ESCAPING_LOCAL_REFERENCE,
                            "Returning a reference into a local, which ends with this function's "
                            "frame");
    }

    if (ret.get_kind() == gs::type_kind::VOID_) {
        builder_.emit_return();
    } else {
        builder_.emit_return(TRY(as_runtime(returned, src)));
    }

    return void_value();
}

auto analyzer::analyze_ret_value(inst_id inst) -> result<operand> {
    const auto& src{src_of(inst)};
    if (!fn_) { return return_outside_function(src); }
    const auto value{TRY(resolve_value(mod().data_of(inst).un.operand, src))};
    auto&      ret{*fn_->return_type};
    if (ret.get_kind() == gs::type_kind::AUTO) { return value; }

    const auto returned{TRY(coerce_to(value, ret, src, {.what = use_site::kind::RETURN}))};
    if (is_comptime() || ret.get_kind() == gs::type_kind::VOID_) { return returned; }
    return operand{TRY(as_runtime(returned, src))};
}

auto analyzer::reject_unlabeled_loop(ref value, const source& src) -> result<void> {
    // Only a labeled loop yields a value, through `break :label v`
    const auto inst{as_inst(value)};
    if (!inst || mod().tag_of(*inst) != tag::loop) { return {}; }
    record_reader rec{record(*inst)};
    if (!rec.str_field().empty()) { return {}; }
    return comp_.report(
        u_,
        src,
        gs::error::TYPE_MISMATCH,
        "A loop without a label has no value; label it and leave it with `break :label value`");
}

auto analyzer::addresses_frame(ref returned) -> bool {
    const auto taken{as_inst(returned)};
    if (!taken ||
        (mod().tag_of(*taken) != tag::ref_of && mod().tag_of(*taken) != tag::address_of)) {
        return false;
    }

    // A field or element lives in its object, which a pointer, reference or slice doesn't
    const auto indirect{[&](inst_id object) {
        const auto found{values_.find(object)};
        if (found == values_.end()) { return false; }
        const auto kind{type_of(found->second).get_kind()};
        return kind == gs::type_kind::POINTER || kind == gs::type_kind::REFERENCE ||
               kind == gs::type_kind::SLICE;
    }};
    const auto object_of{[&](inst_id i) -> stdx::option<inst_id> {
        const auto r{record(i).ref_field()};
        return r ? as_inst(*r) : stdx::none;
    }};
    auto       at{object_of(*taken)};
    while (at) {
        switch (mod().tag_of(*at)) {
        case tag::field_ptr:
        case tag::field_val:
        case tag::elem_ptr:  {
            const auto object{object_of(*at)};
            if (!object || indirect(*object)) { return false; }
            at = object;
            break;
        }
        case tag::elem_val: {
            const auto object{as_inst(mod().data_of(*at).bin.lhs)};
            if (!object || indirect(*object)) { return false; }
            at = object;
            break;
        }
        case tag::load:      at = as_inst(mod().data_of(*at).un.operand); break;
        case tag::alloc_mut:
        case tag::let_value: return true;
        case tag::param:     return !indirect(*at) && !is_comptime_param(*at);
        default:             return false;
        }
    }

    return false;
}

auto analyzer::is_comptime_param(inst_id param) const -> bool {
    record_reader p{record(param)};
    p.skip_str().skip_body().skip_body();
    return (p.u32_field() & flags::PARAM_COMPTIME) != 0;
}

auto analyzer::yields_nothing(gsl::span<const inst_id> body) const -> bool {
    if (body.empty() || mod().tag_of(body.back()) != tag::yield) { return false; }
    return as_well_known(mod().data_of(body.back()).un.operand) == well_known::void_value;
}

auto analyzer::folded_condition(ref cond, const source& src) -> result<stdx::option<bool>> {
    // Only immutable bindings fold, never a call
    if (reaches_call(cond)) { return stdx::option<bool>{}; }
    const auto index{TRY(speculated_value(cond, src))};
    if (!index) { return stdx::option<bool>{}; }
    return comp_.constants().value_at(*index).data.as_opt<bool>().transform(
        [](bool b) { return b; });
}

auto analyzer::speculated_value(ref r, const source& src) -> result<stdx::option<ip::index>> {
    // A failure only means the value stays at runtime, so nothing it records is kept
    const auto before{comp_.speculate()};
    const auto folded{[&] {
        const auto scope{comptime_scope()};
        return reevaluate(r, src);
    }()};
    const bool unsupported{!folded && folded.error() == stop::UNSUPPORTED};
    comp_.abandon(before, unsupported);
    if (unsupported) { return stdx::err{stop::UNSUPPORTED}; }
    if (!folded) { return stdx::option<ip::index>{}; }
    const auto index{comptime_value(*folded)};
    if (!index || comp_.constants().is_type(*index)) { return stdx::option<ip::index>{}; }
    return stdx::option<ip::index>{*index};
}

auto analyzer::known_bool(const operand& op, const source& src) -> result<bool> {
    auto& type{type_of(op)};
    if (type.is_poison()) { return stdx::err{stop::POISON}; }
    if (type.get_kind() != gs::type_kind::BOOL) {
        return comp_.report(
            u_, src, gs::error::TYPE_MISMATCH, messages::condition_not_bool(comp_.ctx(), type));
    }

    const auto index{TRY(require_comptime(op, src))};
    return *comp_.constants().value_at(index).data.as_opt<bool>();
}

auto analyzer::run_block(inst_id inst, gsl::span<const inst_id> body) -> result<operand> {
    const auto out{TRY(analyze_body(body))};
    if (!jump_) { return out.yielded.value_or(void_value()); }
    if (jump_->what != jump::kind::BREAK || jump_->target != inst) { return void_value(); }
    auto value{std::move(*jump_->value)};
    jump_.reset();
    return value;
}

auto analyzer::run_loop(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    rec.skip_str();
    const auto loop_flags{rec.u32_field()};
    const auto cond_body{rec.body_field()};
    const auto body{rec.body_field()};
    const auto cont_body{rec.body_field()};
    const auto else_body{rec.body_field()};
    const bool has_cond{(loop_flags & flags::HAS_COND) != 0};
    const bool do_while{(loop_flags & flags::DO_WHILE) != 0};

    const auto unwrapping{
        (loop_flags & flags::UNWRAPS) != 0
            ? stdx::option<const ast::while_loop_expr&>{u_.source->ast.get_as<ast::while_loop_expr>(
                  *src.as_opt<ast::node_id>())}
            : stdx::none};
    stdx::option<flow_result>  flow;
    stdx::option<unwrap_shape> shape;

    const auto owns{
        [&](jump::kind k) { return jump_ && jump_->what == k && jump_->target == inst; }};
    const auto holds{[&] -> result<bool> {
        const auto scope{comptime_scope()};
        auto       cond{analyze_body(cond_body)};
        if (!cond && cond.error() == stop::RUNTIME && (loop_flags & flags::UNROLL) != 0) {
            return comp_.report(u_,
                                src,
                                gs::error::COMPTIME_WHILE_NONFOLDABLE_COND,
                                "`unroll while`'s condition must fold to a compile-time-known "
                                "`bool`");
        }
        const auto out{TRY(std::move(cond))};
        if (jump_ || !out.yielded) {
            return comp_.unsupported(u_, src, "a loop condition that leaves");
        }

        if (!unwrapping) { return known_bool(*out.yielded, src); }

        // `while (x) |v|` holds while `x.branch()` continues
        const auto cond_ref{mod().data_of(cond_body.back()).un.operand};
        const auto cond_src{as_inst(cond_ref).transform([&](inst_id i) { return src_of(i); })};
        shape.emplace(TRY(capture_shape(
            *out.yielded, unwrapping->else_capture, "while", cond_src.value_or(src), src)));
        flow.emplace(TRY(emit_flow(cond_ref, *out.yielded, *shape, src)));
        return !flow->known->is_break;
    }};

    for (usize iterations{0};; ++iterations) {
        if (has_cond && (!do_while || iterations > 0) && !TRY(holds())) { break; }
        const auto limit{comp_.limits().unroll};
        if (iterations == limit) {
            return comp_.report(u_,
                                src,
                                gs::error::UNROLLED_LOOP_LIMIT,
                                fmt::format("A compile-time loop exceeded its unroll limit of {}; "
                                            "raise it with `@setEvalUnrollLimit`",
                                            limit));
        }

        for (const auto part : {body, cont_body}) {
            if (flow) {
                bind_unwrapped(
                    part, 0, TRY(payload_capture(*unwrapping->payload_capture, *flow, *shape)));
            }

            DISCARD(TRY(analyze_body(part)));

            // An unrolled body that leaves at runtime, like a `return`, ends the loop there
            if (!is_comptime() && !jump_ && terminated()) { return void_value(); }
            if (owns(jump::kind::REPEAT)) {
                jump_.reset();
            } else if (owns(jump::kind::BREAK)) {
                auto value{std::move(*jump_->value)};
                jump_.reset();
                return value;
            } else if (jump_) {
                return void_value();
            }
        }
    }

    if (flow && unwrapping->else_capture) {
        bind_unwrapped(else_body, 1, residual_capture(*flow, *shape));
    }

    return run_block(inst, else_body);
}

auto analyzer::analyze_block_comptime(inst_id inst) -> result<operand> {
    auto rec{record(inst)};
    rec.skip_str();
    const auto body{rec.body_field()};
    if (is_comptime()) { return run_block(inst, body); }

    auto value{[&] {
        const auto scope{comptime_scope()};
        return run_block(inst, body);
    }()};
    // A `return` in the block returns its known value from the runtime function
    const auto& src{src_of(inst)};
    if (jump_ && jump_->what == jump::kind::RETURN && gir_fn_) {
        const auto returned{std::move(*jump_->value)};
        jump_.reset();
        if (fn_->return_type->get_kind() == gs::type_kind::VOID_) {
            builder_.emit_return();
        } else {
            builder_.emit_return(TRY(as_runtime(returned, src)));
        }

        return void_value();
    }

    if (jump_) {
        jump_.reset();
        return comp_.unsupported(u_, src, "a jump out of a comptime block");
    }

    return at_comptime_boundary(std::move(value), src);
}

auto analyzer::analyze_condbr_comptime(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  cond_ref{*rec.ref_field()};
    const auto  then_body{rec.body_field()};
    const auto  else_body{rec.body_field()};
    const auto  cond_src{as_inst(cond_ref).transform([&](inst_id i) { return src_of(i); })};

    auto cond{[&] {
        const auto scope{comptime_scope()};
        return reevaluate(cond_ref, cond_src.value_or(src));
    }()};
    if (!cond && cond.error() == stop::RUNTIME) {
        // Both arms are still checked, so a name either one misspells reports first
        if (!is_comptime()) {
            DISCARD(analyze_unemitted(then_body));
            DISCARD(analyze_unemitted(else_body));
            jump_.reset();
        }
        return comp_.report_runtime_need(
            u_,
            cond_src.value_or(src),
            gs::error::COMPTIME_EVALUATION_FAILED,
            "Comptime if condition could not be evaluated at compile time");
    }

    // Only the arm the condition picks is analyzed
    if (!cond) { return stdx::err{cond.error()}; }
    const bool taken{TRY(known_bool(*cond, cond_src.value_or(src)))};
    const auto out{TRY(analyze_body(taken ? then_body : else_body))};
    if (jump_ || (!is_comptime() && terminated())) { return void_value(); }
    return out.yielded.value_or(void_value());
}

auto analyzer::analyze_eval_branch(inst_id inst) -> result<operand> {
    auto       rec{record(inst)};
    const auto comptime_arm{rec.body_field()};
    const auto runtime_arm{rec.body_field()};
    const auto out{TRY(analyze_body(is_comptime() ? comptime_arm : runtime_arm))};
    if (jump_ || (!is_comptime() && terminated())) { return void_value(); }
    return out.yielded.value_or(void_value());
}

auto analyzer::analyze_unreachable(inst_id inst) -> result<operand> {
    const auto& src{src_of(inst)};
    if (is_comptime()) {
        return comp_.report(
            u_, src, gs::error::UNREACHABLE_CODE_REACHED, "reached unreachable code");
    }

    // With safety checks, reaching it panics
    if (runtime_safety()) {
        TRY(emit_panic("reached unreachable code", src));
    } else {
        builder_.emit_unreachable();
    }

    return comp_.constants().intern(ip::undefined_value{}, builtin(gs::type_kind::NORETURN));
}

} // namespace ghoti::uir::sema
