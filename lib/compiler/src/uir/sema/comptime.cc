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
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/diagnostic.hh"

namespace ghoti::uir::sema {

namespace {

// The notes a diagnostic names its compile-time call stack with, past which they are counted
constexpr usize MAX_CALL_NOTES{8};

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
    if (const auto found{memo_.find(key)}; found != memo_.end()) {
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
    if (!impure && (value || value.error() == stop::POISON)) {
        memo_.emplace(std::move(key), value ? memo_entry{*value} : memo_entry{});
    }

    return value;
}

auto compilation::new_alloc(const comptime_alloc& alloc) -> comptime_place {
    allocs_.emplace_back(alloc);
    return comptime_place{static_cast<u32>(allocs_.size() - 1)};
}

auto compilation::note_store(comptime_place p) -> void {
    for (auto& frame : calls_) {
        if (p.alloc < frame.alloc_base) { frame.impure = true; }
    }
}

auto compilation::speculate() const -> speculation {
    return speculation{
        .diagnostics  = diagnostics_.size(),
        .stopped      = stopped_at_.has_value(),
        .runtime_site = runtime_site_.has_value(),
    };
}

auto compilation::abandon(const speculation& s, bool keep_unsupported) -> void {
    DISCARD(diagnostics_.split_off(s.diagnostics));
    if (!s.stopped && !keep_unsupported) { stopped_at_.reset(); }
    if (!s.runtime_site) { runtime_site_.reset(); }
}

auto compilation::bool_parent(const unit& u, inst_id inst) -> stdx::option<inst_id> {
    if (bool_parents_.size() <= u.index) { bool_parents_.resize(u.index + 1); }
    auto& parents{bool_parents_[u.index]};
    if (!parents) {
        auto&       built{parents.emplace()};
        const auto& m{*u.uir};
        for (u32 i{0}; i < m.instruction_count(); ++i) {
            const inst_id at{i};
            const auto    t{m.tag_of(at)};
            if (t != tag::bool_and && t != tag::bool_or) { continue; }
            record_reader rec{m, m.data_of(at).pl.extra};
            const auto    lhs{as_inst(*rec.ref_field())};
            if (!lhs) { continue; }
            const auto lhs_tag{m.tag_of(*lhs)};
            if (lhs_tag == tag::bool_and || lhs_tag == tag::bool_or) {
                built.insert_or_assign(*lhs, at);
            }
        }
    }

    const auto found{parents->find(inst)};
    if (found == parents->end()) { return stdx::none; }
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
      self_type_{fn.self_type}, frame_{comp.next_frame()}, alloc_base_{comp.alloc_count()} {}

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

        values_.insert_or_assign(param, operand{args[arg++]});
    }

    DISCARD(TRY(analyze_body(body)));
    if (jump_ && jump_->what == jump::kind::RETURN) { return *jump_->value; }
    if (fn_->return_type->get_kind() == gs::type_kind::VOID_) { return void_value(); }
    return comp_.unsupported(
        u_, src_of(fn_->func), "a compile-time call that ends without returning");
}

auto analyzer::require_comptime(const operand& op, const source& src) -> result<ip::index> {
    if (const auto index{comptime_value(op)}) { return *index; }
    if (op.is<function_ref>()) {
        return comp_.unsupported(u_, src, "a function used as a compile-time value");
    }

    return comp_.needs_runtime(u_, src);
}

auto analyzer::at_comptime_boundary(result<operand> value) -> result<operand> {
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
    case tag::slice:    {
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

auto analyzer::reevaluate(ref r, const source& src) -> result<operand> {
    const auto inst{as_inst(r)};
    if (!inst) { return resolve(r, src); }
    const auto found{values_.find(*inst)};
    if (found == values_.end()) { return resolve(r, src); }

    // An aggregate global is its value at compile time, as is a comptime parameter's slot
    if (const auto g{found->second.as_opt<global_const>()}) { return g->value; }
    if (const auto s{found->second.as_opt<slot_value>()}; s && s->known) { return *s->known; }
    if (!found->second.is<gir::value>() && !found->second.is<merge_ref>() &&
        !found->second.is<slot_value>()) {
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
