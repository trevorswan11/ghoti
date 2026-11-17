#include <algorithm>
#include <ranges>
#include <tuple>
#include <utility>
#include <vector>

#include <gsl/util>
#include <stdx/assert.hh>
#include <stdx/option.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/sema/type.hh"
#include "internal.hh"

namespace ghoti::uir::sema {

auto analyzer::void_value() -> operand {
    return comp_.constants().intern(ip::void_value{}, builtin(gs::type_kind::VOID_));
}

auto analyzer::condition(const operand& op, const source& src) -> result<gir::value> {
    auto& type{type_of(op)};
    if (type.is_poison()) { return stdx::err{stop::POISON}; }
    if (type.get_kind() != gs::type_kind::BOOL) {
        return comp_.report(
            u_, src, gs::error::TYPE_MISMATCH, messages::condition_not_bool(comp_.ctx(), type));
    }
    return as_runtime(op, src);
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
    t.arrivals.push_back({.seg = builder_.get_segment()->get_id(), .value = value, .src = src});
    emit_goto(*t.exit);
}

auto analyzer::finish_merge(const std::vector<incoming>& arrivals,
                            gir::segment_id              merge,
                            const source&                src) -> result<operand> {
    switch_to(merge);
    if (arrivals.empty()) {
        builder_.emit_unreachable();
        return void_value();
    }

    std::vector<operand> values;
    values.reserve(arrivals.size());
    bool all_void{true};
    bool all_untyped_constants{true};
    for (const auto& arrival : arrivals) {
        values.push_back(arrival.value);
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
    if (all_untyped_constants) {
        merges_.push_back({.seg = merge, .arrivals = arrivals, .materialized = stdx::none});
        return merge_ref{static_cast<u32>(merges_.size() - 1)};
    }

    auto& peer{*TRY(peer_type(values, src))};
    auto& slot_type{gs::is_comptime_numeric(peer.get_kind()) ? comp_.ctx().default_concrete(peer)
                                                             : peer};
    const auto slot{builder_.emit_alloca_in_entry(slot_type)};
    for (const auto& arrival : arrivals) {
        builder_.set_segment(*gir_fn_->get_segment(arrival.seg));
        builder_.set_splice(true);
        const auto stored{
            coerce_to(arrival.value, slot_type, arrival.src, {.what = use_site::kind::STORE})};
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

auto analyzer::analyze_block(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    DISCARD(rec.str_field());
    const auto body{rec.body_field()};
    if (is_comptime()) {
        const auto out{TRY(analyze_body(body))};
        if (!out.yielded) {
            return comp_.unsupported(u_, src, "a block that leaves at compile time");
        }
        return *out.yielded;
    }

    targets_.push_back({.inst = inst, .exit = stdx::none, .repeat = stdx::none, .arrivals = {}});
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
        const auto known{comptime_value(cond)};
        if (!known) { TRY(require_runtime(src)); }
        TRY(condition(cond, cond_src.value_or(src)));
        const bool taken{*comp_.constants().value_at(*known).data.as_opt<bool>()};
        const auto out{TRY(analyze_body(taken ? then_body : else_body))};
        if (!out.yielded) {
            return comp_.unsupported(u_, src, "a branch that leaves at compile time");
        }
        return *out.yielded;
    }

    // Both arms are analyzed even when the condition is known: only `comptime` prunes (Q4)
    const auto c{TRY(condition(cond, cond_src.value_or(src)))};
    const auto then_seg{new_segment()};
    const auto else_seg{new_segment()};
    const auto merge{new_segment()};
    builder_.emit_cond_goto(c, then_seg, else_seg);

    // With a known condition only the taken arm reaches the merge
    const auto            known{comptime_value(cond).transform(
        [&](ip::index i) { return *comp_.constants().value_at(i).data.as_opt<bool>(); })};
    std::vector<incoming> arrivals;
    for (const auto& [seg, body, taken] :
         {std::tuple{then_seg, then_body, known.value_or(true)},
          std::tuple{else_seg, else_body, !known.value_or(false)}}) {
        switch_to(seg);
        const auto out{TRY(analyze_body(body))};
        if (out.yielded && !terminated()) {
            if (taken) {
                arrivals.push_back(
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
    DISCARD(rec.str_field());
    const auto loop_flags{rec.u32_field()};
    const auto cond_body{rec.body_field()};
    const auto body{rec.body_field()};
    const auto cont_body{rec.body_field()};
    const auto else_body{rec.body_field()};
    if ((loop_flags & (flags::UNROLL | flags::UNWRAPS)) != 0) {
        return comp_.unsupported(u_, src, "an unrolled or unwrapping loop");
    }
    if (is_comptime()) { return comp_.unsupported(u_, src, "a loop at compile time"); }

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
    targets_.push_back({.inst = inst, .exit = exit, .repeat = repeat, .arrivals = {}});
    const auto pop{gsl::finally([this] { targets_.pop_back(); })};

    // The segment the condition's false edge leaves from, for the loop's normal exit
    stdx::option<gir::segment_id> normal_exit;
    const auto                    analyze_cond{[&] -> result<void> {
        switch_to(*cond_seg);
        const auto out{TRY(analyze_body(cond_body))};
        if (!out.yielded || terminated()) {
            return comp_.unsupported(u_, src, "a loop condition that leaves");
        }
        const auto c{TRY(condition(*out.yielded, src))};
        normal_exit = builder_.get_segment()->get_id();
        builder_.emit_cond_goto(c, body_seg, else_seg.value_or(exit));
        return {};
    }};

    emit_goto(has_cond && !do_while ? *cond_seg : body_seg);
    if (has_cond && !do_while) { TRY(analyze_cond()); }

    switch_to(body_seg);
    DISCARD(TRY(analyze_body(body)));
    if (!terminated()) { emit_goto(repeat); }
    if (cont_seg) {
        switch_to(*cont_seg);
        DISCARD(TRY(analyze_body(cont_body)));
        if (!terminated()) { emit_goto(cond_seg.value_or(body_seg)); }
    }
    if (has_cond && do_while) { TRY(analyze_cond()); }

    stdx::option<incoming> normal;
    if (else_seg) {
        switch_to(*else_seg);
        const auto out{TRY(analyze_body(else_body))};
        if (out.yielded && !terminated()) {
            normal.emplace(builder_.get_segment()->get_id(), *out.yielded, src);
            emit_goto(exit);
        }
    } else if (normal_exit) {
        normal.emplace(*normal_exit, void_value(), src);
    }
    auto arrivals{targets_.back().arrivals};
    if (normal) { arrivals.push_back(std::move(*normal)); }
    return finish_merge(arrivals, exit, src);
}

auto analyzer::analyze_break(inst_id inst) -> result<operand> {
    const auto& d{mod().data_of(inst)};
    const auto& src{src_of(inst)};
    if (is_comptime()) { return comp_.unsupported(u_, src, "a break at compile time"); }
    const auto value{TRY(resolve_value(d.bin.rhs, src))};
    arrive(target_of(d.bin.lhs), value, src);
    return void_value();
}

auto analyzer::analyze_repeat(inst_id inst) -> result<operand> {
    const auto& src{src_of(inst)};
    if (is_comptime()) { return comp_.unsupported(u_, src, "a continue at compile time"); }
    const auto& t{target_of(mod().data_of(inst).un.operand)};
    emit_goto(*t.repeat);
    return void_value();
}

auto analyzer::analyze_ret(inst_id inst) -> result<operand> {
    const auto& src{src_of(inst)};
    if (is_comptime()) { return comp_.unsupported(u_, src, "a return at compile time"); }
    const auto value{TRY(resolve_value(mod().data_of(inst).un.operand, src))};
    auto&      ret{*fn_->return_type};
    const auto returned{TRY(coerce_to(value, ret, src, {.what = use_site::kind::RETURN}))};
    if (ret.get_kind() == gs::type_kind::VOID_) {
        builder_.emit_return();
    } else {
        builder_.emit_return(TRY(as_runtime(returned, src)));
    }
    return void_value();
}

} // namespace ghoti::uir::sema
