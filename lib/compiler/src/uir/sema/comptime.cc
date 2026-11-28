#include <algorithm>
#include <bit>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/span>
#include <gsl/util>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/diagnostic.hh"

namespace ghoti::uir::sema {

namespace {

// The notes a diagnostic names its compile-time call stack with, past which they are counted
constexpr usize MAX_CALL_NOTES{8};

// Whether a value holds a compile-time address, whose memory a call may read or change
[[nodiscard]] auto holds_memory(const ip::pool& pool, ip::index at) -> bool {
    if (pool.is_type(at)) { return false; }
    const auto& data{pool.value_at(at).data};
    if (data.is<ip::memory_value>()) { return true; }
    if (const auto agg{data.as_opt<ip::aggregate_value>()}) {
        return std::ranges::any_of(
            agg->elements, [&](stdx::option<ip::index> e) { return e && holds_memory(pool, *e); });
    }

    if (const auto un{data.as_opt<ip::union_value>()}) {
        return std::ranges::any_of(un->payload, [&](ip::index e) { return holds_memory(pool, e); });
    }

    return false;
}

} // namespace

auto memo_key_hash::operator()(const memo_key& key) const noexcept -> u64 {
    stdx::hasher h{static_cast<u64>(std::bit_cast<uptr>(key.fn))};
    for (const auto arg : key.args) { h.combine(std::to_underlying(arg)); }
    return h.finalize();
}

auto compilation::call_comptime(const unit&            caller,
                                const source&          src,
                                instance&              fn,
                                std::vector<ip::index> args) -> result<operand> {
    PROFILE_FUNCTION();
    memo_key key{.fn = &fn, .args = std::move(args)};

    // A call given compile-time memory may read what was stored there since
    const bool borrows{std::ranges::any_of(
        key.args, [&](ip::index arg) { return holds_memory(constants(), arg); })};
    if (const auto found{memo_.find(key)}; !borrows && found != memo_.end()) {
        if (!found->second) { return stdx::err{stop::POISON}; }
        return *found->second;
    }

    if (calls_.size() >= limits_.recursion) {
        return report(caller,
                      src,
                      gs::error::COMPTIME_RECURSION_LIMIT_EXCEEDED,
                      "Compile-time function recursion limit exceeded");
    }

    calls_.emplace_back<comptime_frame>({
        .caller     = &caller,
        .call_src   = src,
        .callee     = fn.symbol,
        .key        = &key,
        .alloc_base = alloc_count(),
    });
    const auto saved{limits_};
    auto       value{[&] {
        analyzer a{*this, *fn.owner, fn};
        return a.eval_call(key.args);
    }()};
    limits_ = saved;
    const bool impure{calls_.back().impure};
    calls_.pop_back();

    // A call that needed a runtime value or stopped early is retried by its next use
    if (!impure && !borrows && (value || value.error() == stop::POISON)) {
        memo_.emplace(std::move(key), value ? memo_entry{*value} : memo_entry{});
    }

    return value;
}

auto compilation::new_alloc(const comptime_alloc& alloc) -> comptime_place {
    allocs_.emplace_back(alloc);
    return comptime_place{static_cast<u32>(allocs_.size() - 1)};
}

auto compilation::note_store(const comptime_place& p) -> void {
    for (auto& frame : calls_) {
        if (p.alloc < frame.alloc_base) { frame.impure = true; }
    }
}

compilation::speculation::speculation(compilation& comp) noexcept
    : diagnostics{comp.diagnostics_.size()}, memo{comp.memo_.size()},
      stopped{comp.stopped_at_.has_value()}, runtime_site{comp.runtime_site_.has_value()},
      owner{comp} {
    ++comp.speculating_;
}

compilation::speculation::~speculation() { --owner.speculating_; }

auto compilation::speculate() -> speculation { return speculation{*this}; }

auto compilation::drop_value_errors(usize from) -> bool {
    auto rest{diagnostics_.split_off(from)};
    bool dropped{false};
    for (const auto& d : rest) {
        const auto e{d.get_error()};
        if (e != gs::error::COMPTIME_EVALUATION_FAILED && e != gs::error::LITERAL_OUT_OF_RANGE) {
            diagnostics_.push_back(d);
        } else {
            dropped = true;
        }
    }

    return dropped;
}

auto compilation::abandon(const speculation& s, bool keep_unsupported) -> void {
    DISCARD(diagnostics_.split_off(s.diagnostics));

    // A call that failed here lost its error with the diagnostics, so its next use runs it again
    std::vector<memo_key> failed;
    for (usize i{s.memo}; i < memo_.values().size(); ++i) {
        if (!memo_.values()[i].second) { failed.emplace_back(memo_.values()[i].first); }
    }
    for (const auto& key : failed) { memo_.erase(key); }
    if (!s.stopped && !keep_unsupported) { stopped_at_.reset(); }
    if (!s.runtime_site) { runtime_site_.reset(); }
}

auto compilation::bool_ops_starting_at(const unit& u, inst_id inst) -> gsl::span<const inst_id> {
    if (bool_starts_.size() <= u.index) { bool_starts_.resize(u.index + 1); }
    auto& starts{bool_starts_[u.index]};
    if (!starts) {
        auto&       built{starts.emplace()};
        const auto& m{*u.uir};
        for (u32 i{0}; i < m.instruction_count(); ++i) {
            const inst_id at{i};
            const auto    t{m.tag_of(at)};
            if (t != tag::bool_and && t != tag::bool_or) { continue; }
            record_reader rec{m, m.data_of(at).pl.extra};
            rec.skip_ref().skip_body();
            if (const auto start{rec.u32_field()}; start != 0) {
                built[inst_id{start - 1}].emplace_back(at);
            }
        }

        // An enclosing `and` / `or` is lowered after the ones its left side holds
        for (auto& [start, ops] : built) { std::ranges::sort(ops, std::ranges::greater{}); }
    }

    const auto found{starts->find(inst)};
    if (found == starts->end()) { return {}; }
    return found->second;
}

auto compilation::needs_runtime(const unit& u, const source& src) -> stdx::err<stop> {
    if (!runtime_site_) { runtime_site_.emplace(&u, src); }
    return stdx::err{stop::RUNTIME};
}

auto compilation::note_runtime_let(const unit& u, std::string_view name, const source& init)
    -> void {
    if (!runtime_site_ || runtime_site_->through_let) { return; }
    runtime_site_->through_let.emplace(
        fmt::format("'{}' could not be evaluated at compile time", name),
        located_span{u.source->path, span_of(u, init)});
}

auto compilation::report_runtime_need(const unit&                 u,
                                      const stdx::option<source>& at,
                                      gs::error                   code,
                                      std::string                 message) -> stdx::err<stop> {
    auto       site{std::exchange(runtime_site_, stdx::none)};
    const auto note{site ? site->through_let : stdx::option<diagnostic_note>{}};
    if (at) { return report(u, *at, code, std::move(message), note); }
    if (site) { return report(*site->u, site->src, code, std::move(message), note); }
    return stdx::err{stop::POISON};
}

auto compilation::add_call_notes(gs::diagnostic& d) const -> void {
    usize written{0};
    for (const auto& frame : calls_ | std::views::reverse) {
        if (written == MAX_CALL_NOTES) {
            d.with_note(fmt::format("... {} more", calls_.size() - MAX_CALL_NOTES));
            return;
        }

        d.with_note(
            fmt::format("in the compile-time call to '{}'", frame.callee),
            located_span{frame.caller->source->path, span_of(*frame.caller, frame.call_src)});
        ++written;
    }
}

analyzer::analyzer(compilation& comp, const unit& u, instance& fn) noexcept
    : comp_{comp}, u_{u}, fn_{fn}, captures_{fn.captures}, scope_{fn.scope},
      self_type_{fn.self_type}, frame_{comp.next_frame()}, alloc_base_{comp.alloc_count()} {
    env_values_ = fn.comptime_env;
    env_frame_  = fn.env_frame;
}

auto analyzer::eval_call(gsl::span<const ip::index> args) -> result<operand> {
    PROFILE_FUNCTION();
    record_reader rec{record(fn_->func)};
    const auto    params{params_of(mod(), rec.body_field())};
    rec.skip_body();
    const auto body{rec.body_field()};
    for (usize i{0}, arg{0}; const auto param : params) {
        const auto binding{fn_->bindings.empty() ? stdx::option<param_binding>{}
                                                 : stdx::option<param_binding>{fn_->bindings[i]}};
        ++i;
        if (binding && binding->value) {
            values_.insert_or_assign(param, operand{*binding->value});
            if (binding->has_slot) { ++arg; }
            continue;
        }

        // A pack takes one argument per value it holds
        if (binding && binding->pack_size) {
            std::vector<operand> elements;
            for (usize k{0}; k < *binding->pack_size; ++k) { elements.emplace_back(args[arg++]); }
            values_.insert_or_assign(param, pack_ref{static_cast<u32>(packs_.size())});
            packs_.emplace_back(std::move(elements));
            continue;
        }

        // A parameter the body writes lives in compile-time memory
        record_reader p{record(param)};
        p.skip_str().skip_body().skip_body();
        if ((p.u32_field() & flags::PARAM_WRITTEN) != 0) {
            const auto index{args[arg++]};
            auto&      type{type_of(operand{index})};
            values_.insert_or_assign(param,
                                     operand{comp_.new_alloc({
                                         .type                = type,
                                         .value               = index,
                                         .frame               = frame_,
                                         .runtime_depth       = runtime_depth_,
                                         .keeps_comptime_type = false,
                                     })});
            continue;
        }

        values_.insert_or_assign(param, operand{args[arg++]});
    }

    DISCARD(TRY(analyze_body(body)));
    if (jump_ && jump_->what == jump::kind::RETURN) { return *jump_->value; }
    if (fn_->return_type->get_kind() == gs::type_kind::VOID_) { return void_value(); }
    return comp_.report(u_,
                        src_of(fn_->func),
                        gs::error::RETURN_TYPE_MISMATCH,
                        fmt::format("Function returning '{}' never returns a value",
                                    comp_.ctx().type_display_name(*fn_->return_type)));
}

auto analyzer::require_comptime(const operand& op, const source& src) -> result<ip::index> {
    if (const auto index{comptime_value(op)}) { return *index; }
    // A function's compile-time value is its symbol, which it is called and exported by
    if (const auto fn{op.as_opt<function_ref>()}) { return comp_.function_value(*fn->fn); }

    return comp_.needs_runtime(u_, src);
}

auto analyzer::at_comptime_boundary(result<operand> value, const source& src) -> result<operand> {
    // A `@skip` the evaluation ran skips the test here
    if (!value && value.error() == stop::SKIP) {
        auto&      ctx{comp_.ctx()};
        auto&      text_type{ctx.get_slice(gs::types::mut::CONSTANT, false, ctx.get_int(8, false))};
        const auto message{comp_.constants().intern(comp_.take_skip(), text_type)};
        if (!gir_fn_) { return comp_.unsupported(u_, src, "a `@skip` outside a test"); }
        return emit_skip(constant(message, text_type), src);
    }

    if (value || value.error() != stop::RUNTIME) { return value; }
    return comp_.report_runtime_need(u_,
                                     stdx::none,
                                     gs::error::COMPTIME_EVALUATION_FAILED,
                                     "unable to evaluate at compile time");
}

namespace {

// The operands an instruction reads, for the instructions a comptime read may run again
[[nodiscard]] auto reevaluable_operands(const module_t& m, inst_id inst)
    -> stdx::option<std::vector<ref>> {
    const auto& d{m.data_of(inst)};
    switch (m.tag_of(inst)) {
    case tag::add:
    case tag::sub:
    case tag::mul:
    case tag::div:
    case tag::mod:
    case tag::add_wrap:
    case tag::sub_wrap:
    case tag::mul_wrap:
    case tag::add_sat:
    case tag::sub_sat:
    case tag::mul_sat:
    case tag::shl_sat:
    case tag::shl_wrap:
    case tag::shl:
    case tag::shr:
    case tag::bit_and:
    case tag::bit_or:
    case tag::bit_xor:
    case tag::cmp_eq:
    case tag::cmp_ne:
    case tag::cmp_lt:
    case tag::cmp_le:
    case tag::cmp_gt:
    case tag::cmp_ge:    return std::vector{d.bin.lhs, d.bin.rhs};
    case tag::neg:
    case tag::neg_wrap:
    case tag::pos:
    case tag::bool_not:
    case tag::bit_not:   return std::vector{d.un.operand};
    case tag::let_value: {
        record_reader rec{m, d.pl.extra};
        rec.skip_str().skip_ref();
        return std::vector{*rec.ref_field()};
    }
    case tag::call: {
        record_reader rec{m, d.pl.extra};
        std::vector   refs{*rec.ref_field()};
        if (rec.ref_field()) { return stdx::none; }
        for (const auto arg : rec.refs_field()) { refs.emplace_back(arg); }
        return refs;
    }
    case tag::field_call: {
        record_reader rec{m, d.pl.extra};
        std::vector   refs{*rec.ref_field()};
        rec.skip_str().skip_u32();
        if (rec.ref_field()) { return stdx::none; }
        for (const auto arg : rec.refs_field()) { refs.emplace_back(arg); }
        return refs;
    }
    case tag::elem_val: return std::vector{d.bin.lhs, d.bin.rhs};
    case tag::load:     return std::vector{d.un.operand};

    // A closure is built again from what it captures
    case tag::closure:  {
        record_reader rec{m, d.pl.extra};
        rec.skip_ref();
        const auto       count{rec.count()};
        std::vector<ref> refs;
        for (u32 i{0}; i < count; ++i) {
            refs.emplace_back(*rec.ref_field());
            rec.skip_u32().skip_str();
        }

        return refs;
    }

    // A cast computes nothing but its value
    case tag::builtin: {
        record_reader rec{m, d.pl.extra};
        switch (static_cast<syntax::builtin_t>(rec.u32_field())) {
        case syntax::builtin_t::INT_CAST:
        case syntax::builtin_t::TRUNCATE:
        case syntax::builtin_t::BIT_CAST:
        case syntax::builtin_t::INT_FROM_BOOL:
        case syntax::builtin_t::BOOL_FROM_INT:
        case syntax::builtin_t::FROM_BACKING_INT:
        case syntax::builtin_t::BACKING_INT:
        case syntax::builtin_t::INT_FROM_FLOAT:
        case syntax::builtin_t::FLOAT_FROM_INT:
        case syntax::builtin_t::FLOAT_CAST:
        case syntax::builtin_t::AS:               {
            const auto args{rec.refs_field()};
            return std::vector<ref>{args.begin(), args.end()};
        }
        default: return stdx::none;
        }
    }
    case tag::elem_ptr: {
        record_reader rec{m, d.pl.extra};
        const auto    object{*rec.ref_field()};
        return std::vector{object, *rec.ref_field()};
    }
    case tag::field_ptr: {
        record_reader rec{m, d.pl.extra};
        return std::vector{*rec.ref_field()};
    }
    case tag::slice: {
        record_reader    rec{m, d.pl.extra};
        std::vector<ref> refs{*rec.ref_field()};
        for (const auto bound : {rec.ref_field(), rec.ref_field()}) {
            if (bound) { refs.emplace_back(*bound); }
        }

        return refs;
    }

    // The right side's body runs again at compile time with the rest of the instruction
    case tag::bool_and:
    case tag::bool_or:  {
        record_reader rec{m, d.pl.extra};
        return std::vector{*rec.ref_field()};
    }
    case tag::field_val: {
        record_reader rec{m, d.pl.extra};
        return std::vector{*rec.ref_field()};
    }
    case tag::init_struct: {
        record_reader    rec{m, d.pl.extra};
        std::vector<ref> refs;
        rec.skip_ref();
        const auto count{rec.count()};
        for (u32 i{0}; i < count; ++i) {
            rec.skip_str();
            refs.emplace_back(*rec.ref_field());
        }

        return refs;
    }
    case tag::init_array: {
        record_reader rec{m, d.pl.extra};
        rec.skip_ref();
        const auto values{rec.refs_field()};
        return std::vector<ref>{values.begin(), values.end()};
    }
    case tag::array_lit: {
        record_reader rec{m, d.pl.extra};
        rec.skip_ref().skip_ref().skip_u32();
        const auto items{rec.refs_field()};
        return std::vector<ref>{items.begin(), items.end()};
    }
    default: return stdx::none;
    }
}

} // namespace

auto analyzer::operands_known(inst_id inst) const -> bool {
    const auto operands{reevaluable_operands(mod(), inst)};
    if (!operands) { return false; }
    return std::ranges::all_of(*operands, [&](ref r) {
        const auto operand_inst{as_inst(r)};
        if (!operand_inst || mod().tag_of(*operand_inst) == tag::str) { return true; }
        const auto found{values_.find(*operand_inst)};
        if (found == values_.end()) { return false; }
        const auto& op{found->second};
        if (comptime_value(op) || op.is<global_const>() || op.is<function_ref>() ||
            op.is<generic_ref>() || op.is<module_ref>() || op.is<enum_literal>()) {
            return true;
        }

        // A vector built at runtime from a known value
        if (op.is<gir::value>() && built_value(op)) { return true; }

        // An immutable binding of a known value
        if (const auto bound{op.as_opt<slot_value>()};
            bound && !bound->is_mutable && bound->known) {
            return true;
        }

        // An immutable member of a module or type, like `builtin.cpu`, is a constant too
        const auto slot{op.as_opt<place>()};
        if (!slot || slot->is_mutable || mod().tag_of(*operand_inst) != tag::field_ptr) {
            return false;
        }

        record_reader rec{mod(), mod().data_of(*operand_inst).pl.extra};
        const auto    owner{as_inst(*rec.ref_field())};
        if (!owner) { return true; }
        const auto held{values_.find(*owner)};
        if (held == values_.end()) { return false; }
        const auto index{comptime_value(held->second)};
        const auto bound{held->second.as_opt<slot_value>()};
        return held->second.is<module_ref>() || (index && comp_.constants().is_type(*index)) ||
               (bound && bound->global_value);
    });
}

auto analyzer::reaches_call(ref r) const -> bool {
    const auto inst{as_inst(r)};
    if (!inst || !values_.contains(*inst)) { return false; }
    const auto t{mod().tag_of(*inst)};
    if (t == tag::call || t == tag::field_call) { return true; }

    // An address's number is only known at runtime
    if (t == tag::builtin) {
        record_reader rec{record(*inst)};
        if (static_cast<syntax::builtin_t>(rec.u32_field()) == syntax::builtin_t::INT_FROM_PTR) {
            return true;
        }
    }
    const auto operands{reevaluable_operands(mod(), *inst)};
    return operands && std::ranges::any_of(*operands, [&](ref o) { return reaches_call(o); });
}

auto analyzer::reevaluate(ref r, const source& src) -> result<operand> {
    const auto inst{as_inst(r)};
    if (!inst) { return resolve(r, src); }
    const auto found{values_.find(*inst)};
    if (found == values_.end()) { return resolve(r, src); }

    // An aggregate global is its value at compile time
    if (const auto g{found->second.as_opt<global_const>()}) { return g->value; }
    if (const auto s{found->second.as_opt<slot_value>()}; s && s->known) { return *s->known; }
    // A vector built at runtime is known at compile time
    if (found->second.is<gir::value>()) {
        if (const auto built{built_value(found->second)}) { return operand{*built}; }
    }

    // A field or element address is computed again on its object's compile-time value
    const auto t{mod().tag_of(*inst)};
    const bool address{found->second.is<place>() && (t == tag::field_ptr || t == tag::elem_ptr)};
    if (!found->second.is<gir::value>() && !found->second.is<merge_ref>() &&
        !found->second.is<slot_value>() && !address) {
        return found->second;
    }

    const auto& inst_src{src_of(*inst)};
    const auto  operands{reevaluable_operands(mod(), *inst)};
    if (!operands) { return comp_.needs_runtime(u_, inst_src); }

    // Each runtime operand is run again first, then this instruction on their comptime values
    std::vector<std::pair<inst_id, operand>> saved;
    const auto                               restore{gsl::finally([&] {
        for (auto& [i, value] : saved) { values_.insert_or_assign(i, std::move(value)); }
    })};
    for (const auto operand_ref : *operands) {
        const auto operand_inst{as_inst(operand_ref)};
        if (!operand_inst || !values_.contains(*operand_inst)) { continue; }
        auto value{reevaluate(operand_ref, inst_src)};
        if (!value) {
            if (value.error() == stop::RUNTIME && mod().tag_of(*inst) == tag::let_value) {
                record_reader rec{record(*inst)};
                comp_.note_runtime_let(u_, rec.str_field(), src_of(*operand_inst));
            }

            return stdx::err{value.error()};
        }

        saved.emplace_back(*operand_inst, values_.at(*operand_inst));
        values_.insert_or_assign(*operand_inst, std::move(*value));
    }

    const auto scope{comptime_scope()};
    return analyze_inst(*inst);
}

} // namespace ghoti::uir::sema
