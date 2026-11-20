#include <algorithm>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/pointers>
#include <gsl/span>
#include <gsl/util>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/big_int.hh"
#include "support/int128.hh"

namespace ghoti::uir::sema {

namespace {

using gir::instruction_kind;

// A range's bounds as its record lists them
struct range_bounds {
    stdx::option<ref> lo;
    stdx::option<ref> hi;
    bool              inclusive;
};

[[nodiscard]] auto bounds_of(const module_t& m, inst_id range) -> range_bounds {
    record_reader rec{m, m.data_of(range).pl.extra};
    const auto    lo{rec.ref_field()};
    const auto    hi{rec.ref_field()};
    return {.lo = lo, .hi = hi, .inclusive = (rec.u32_field() & flags::INCLUSIVE) != 0};
}

} // namespace

auto analyzer::analyze_range(inst_id inst) -> result<operand> {
    // A range only means something to the `for` that reads it
    DISCARD(inst);
    return void_value();
}

auto analyzer::analyze_bind(inst_id inst) -> result<operand> {
    // What a construct binds is set before its body runs
    if (const auto found{values_.find(inst)}; found != values_.end()) { return found->second; }
    return comp_.unsupported(u_, src_of(inst), "a binding outside its construct");
}

auto analyzer::usize_bound(ref r, const source& src) -> result<gir::value> {
    const auto value{TRY(resolve_value(r, src))};
    const auto coerced{
        TRY(coerce_to(value, builtin(gs::type_kind::USIZE), src, {.what = use_site::kind::STORE}))};
    return as_runtime(coerced, src);
}

auto analyzer::analyze_slice(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  object_ref{*rec.ref_field()};
    const auto  lo_ref{rec.ref_field()};
    const auto  hi_ref{rec.ref_field()};
    const bool  inclusive{(rec.u32_field() & flags::INCLUSIVE) != 0};
    if (is_comptime()) { return comp_.unsupported(u_, src, "slicing at compile time"); }

    auto&      ctx{comp_.ctx()};
    auto&      usize_type{builtin(gs::type_kind::USIZE)};
    auto&      bool_type{builtin(gs::type_kind::BOOL)};
    const auto lo{lo_ref ? TRY(usize_bound(*lo_ref, src)) : usize_constant(0)};

    auto base{TRY(address_of(object_ref, src))};
    if (const auto r{base.pointee->get_data().as_opt<gs::types::reference>()}) {
        base = place{.address = load(base), .pointee = &r->underlying, .is_mutable = false};
    }

    auto&      source_type{*base.pointee};
    const auto elem{[&] -> stdx::option<gs::type&> {
        if (const auto arr{source_type.get_data().as_opt<gs::types::array>()}) {
            return arr->underlying;
        }

        if (const auto s{source_type.get_data().as_opt<gs::types::slice>()}) {
            return s->underlying;
        }

        return stdx::none;
    }()};
    if (!elem) { return comp_.unsupported(u_, src, "slicing this type"); }
    const auto mut{source_type.is_constant() ? gs::types::mut::CONSTANT : gs::types::mut::MUTABLE};
    auto&      result_type{ctx.get_slice(mut, false, *elem)};
    auto&      ptr_type{ctx.get_pointer(mut, *elem)};

    gir::value base_ptr;
    gir::value length;
    if (const auto arr{source_type.get_data().as_opt<gs::types::array>()}) {
        base_ptr = gir::value{
            builder_.emit_get_element_ptr(base.address, {usize_constant(0)}, ptr_type), ptr_type};
        length = usize_constant(arr->len);
    } else {
        const auto ptr_slot{builder_.emit_get_element_ptr(
            base.address, {usize_constant(SLICE_PTR_FIELD)}, ptr_type)};
        base_ptr =
            gir::value{builder_.emit_load(gir::value{ptr_slot, ptr_type}, ptr_type), ptr_type};
        const auto len_slot{builder_.emit_get_element_ptr(
            base.address, {usize_constant(SLICE_LEN_FIELD)}, usize_type)};
        length = gir::value{builder_.emit_load(gir::value{len_slot, usize_type}, usize_type),
                            usize_type};
    }

    auto hi{hi_ref ? TRY(usize_bound(*hi_ref, src)) : length};
    if (hi_ref && inclusive) {
        hi = gir::value{
            builder_.emit_binary(instruction_kind::ADD, hi, usize_constant(1), usize_type),
            usize_type};
    }

    if (ctx.runtime_safety) {
        const auto lo_le_hi{builder_.emit_binary(instruction_kind::LE, lo, hi, bool_type)};
        const auto hi_le_len{builder_.emit_binary(instruction_kind::LE, hi, length, bool_type)};
        const auto in_bounds{builder_.emit_binary(instruction_kind::AND,
                                                  gir::value{lo_le_hi, bool_type},
                                                  gir::value{hi_le_len, bool_type},
                                                  bool_type)};
        TRY(guard(gir::value{in_bounds, bool_type}, "slice range out of bounds", src));
    }

    const auto new_ptr{builder_.emit_get_element_ptr(base_ptr, {lo}, ptr_type)};
    const auto new_len{builder_.emit_binary(instruction_kind::SUB, hi, lo, usize_type)};
    const auto slot{builder_.emit_alloca(result_type)};
    const auto ptr_field{builder_.emit_get_element_ptr(
        gir::value{slot, result_type}, {usize_constant(SLICE_PTR_FIELD)}, ptr_type)};
    builder_.emit_store(gir::value{ptr_field, ptr_type}, gir::value{new_ptr, ptr_type})
        .is_initializer = true;
    const auto len_field{builder_.emit_get_element_ptr(
        gir::value{slot, result_type}, {usize_constant(SLICE_LEN_FIELD)}, usize_type)};
    builder_.emit_store(gir::value{len_field, usize_type}, gir::value{new_len, usize_type})
        .is_initializer = true;
    return gir::value{builder_.emit_load(gir::value{slot, result_type}, result_type), result_type};
}

auto analyzer::known_range(const range_operands& r, bool inclusive) -> stdx::option<value_range> {
    const auto known{[&](const stdx::option<operand>& bound) -> stdx::option<big_int> {
        const auto index{bound ? comptime_value(*bound) : stdx::none};
        return index ? int_of(comp_.constants().value_at(*index)) : stdx::none;
    }};
    const auto start{r.lo ? known(r.lo) : stdx::option<big_int>{big_int{i128{0}}}};
    const auto end{known(r.hi)};
    if (!start || !end) { return stdx::none; }
    // The last value is the end itself only for `..=`
    const auto one{big_int{i128{1}}};
    if (*start > *end) { return value_range{.lo = inclusive ? *end : *end + one, .hi = *start}; }
    if (*start == *end) { return value_range{.lo = *start, .hi = *start}; }
    return value_range{.lo = *start, .hi = inclusive ? *end : *end - one};
}

auto analyzer::counter_type(const range_operands& r) -> gs::type& {
    auto& usize_type{builtin(gs::type_kind::USIZE)};

    // A bare literal bound counts in the other bound's type
    gs::type* elem{r.lo ? &type_of(*r.lo) : &usize_type};
    if (r.lo && r.hi && r.lo_is_literal && gs::is_integer(type_of(*r.hi).get_kind())) {
        elem = &type_of(*r.hi);
    }

    // Untyped bounds count in `usize`, or in `isize` once one is negative
    if (elem->get_kind() == gs::type_kind::COMPTIME_INT &&
        (!r.hi || type_of(*r.hi).get_kind() == gs::type_kind::COMPTIME_INT)) {
        const auto negative{[&](const stdx::option<operand>& bound) {
            const auto index{bound ? comptime_value(*bound) : stdx::none};
            const auto value{index ? int_of(comp_.constants().value_at(*index)) : stdx::none};
            return value && value->is_negative();
        }};
        elem = &builtin(negative(r.lo) || negative(r.hi) ? gs::type_kind::ISIZE
                                                         : gs::type_kind::USIZE);
    }

    return comp_.ctx().default_concrete(*elem);
}

auto analyzer::run_for(inst_id                                   inst,
                       gsl::span<const ref>                      iterables,
                       const std::vector<stdx::option<inst_id>>& binds,
                       gsl::span<const inst_id>                  body,
                       gsl::span<const inst_id>                  else_body) -> result<operand> {
    const auto& src{src_of(inst)};
    auto&       pool{comp_.constants()};

    // Each iterable's values: a range counts from its start, a container lists its elements
    struct source_values {
        stdx::option<big_int>    start;
        bool                     descending{false};
        gsl::not_null<gs::type*> elem;
        std::vector<ip::index>   elements;
    };
    std::vector<source_values> sources;
    stdx::option<usize>        length;
    const auto bound_by{[&](usize n) { length = length ? std::min(*length, n) : n; }};
    const auto known_int{[&](const operand& op) -> result<big_int> {
        return *int_of(pool.value_at(TRY(require_comptime(op, src))));
    }};
    for (const auto iterable_ref : iterables) {
        const auto iterable_inst{as_inst(iterable_ref)};
        if (iterable_inst && mod().tag_of(*iterable_inst) == tag::range) {
            const auto     bounds{bounds_of(mod(), *iterable_inst)};
            range_operands r{};
            if (bounds.lo) {
                r.lo.emplace(TRY(resolve_value(*bounds.lo, src)));
                const auto lo_inst{as_inst(*bounds.lo)};
                r.lo_is_literal = lo_inst && mod().tag_of(*lo_inst) == tag::int_value;
            }

            if (bounds.hi) { r.hi.emplace(TRY(resolve_value(*bounds.hi, src))); }
            auto&         elem{counter_type(r)};
            const auto    start{r.lo ? TRY(known_int(*r.lo)) : big_int{i128{0}}};
            source_values values{
                .start = start, .descending = false, .elem = &elem, .elements = {}};
            if (r.hi) {
                const auto end{TRY(known_int(*r.hi))};
                values.descending = start > end;
                auto span{values.descending ? start - end : end - start};
                if (bounds.inclusive) { span = span + big_int{i128{1}}; }
                bound_by(static_cast<usize>(*span.to_u128()));
            }

            sources.emplace_back(std::move(values));
            continue;
        }

        const auto  given{TRY(resolve_value(iterable_ref, src))};
        const auto  container{TRY(require_comptime(given, src))};
        const auto& value{pool.value_at(container)};
        const auto  agg{value.data.as_opt<ip::aggregate_value>()};
        const auto arr{value.type ? value.type->get_data().as_opt<gs::types::array>() : stdx::none};
        if (!agg || !arr) { return comp_.unsupported(u_, src, "a comptime `for` over this value"); }
        source_values values{
            .start = stdx::none, .descending = false, .elem = &arr->underlying, .elements = {}};
        for (const auto element : agg->elements) {
            values.elements.emplace_back(
                element.value_or(pool.intern(ip::undefined_value{}, arr->underlying)));
        }

        bound_by(values.elements.size());
        sources.emplace_back(std::move(values));
    }

    if (!length) { return comp_.unsupported(u_, src, "a comptime `for` with no end"); }

    const auto owns{
        [&](jump::kind k) { return jump_ && jump_->what == k && jump_->target == inst; }};
    for (usize i{0}; i < *length; ++i) {
        const auto limit{comp_.limits().unroll};
        if (i == limit) {
            return comp_.report(u_,
                                src,
                                gs::error::UNROLLED_LOOP_LIMIT,
                                fmt::format("A compile-time loop exceeded its unroll limit of {}; "
                                            "raise it with `@setEvalUnrollLimit`",
                                            limit));
        }

        for (usize s{0}; s < sources.size(); ++s) {
            if (!binds[s]) { continue; }
            const auto& source{sources[s]};
            if (source.start) {
                const auto step{big_int{static_cast<i128>(i)}};
                const auto current{source.descending ? *source.start - step : *source.start + step};
                values_.insert_or_assign(*binds[s], intern_int(pool, current, *source.elem));
            } else {
                values_.insert_or_assign(*binds[s], source.elements[i]);
            }
        }

        DISCARD(TRY(analyze_body(body)));
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

    return run_block(inst, else_body);
}

auto analyzer::analyze_for(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    rec.skip_str();
    const auto loop_flags{rec.u32_field()};
    const auto iterables{rec.refs_field()};
    const auto body{rec.body_field()};
    const auto else_body{rec.body_field()};
    if ((loop_flags & flags::UNROLL) != 0) { return comp_.unsupported(u_, src, "an `unroll for`"); }

    // The names the body binds, by the iterable each comes from
    std::vector<stdx::option<inst_id>> binds(iterables.size());
    for (const auto i : body) {
        if (mod().tag_of(i) != tag::bind) { continue; }
        record_reader b{record(i)};

        // `|_|` binds nothing
        if (b.str_field() == "_") { continue; }
        const auto slot{b.u32_field()};
        if (b.u32_field() != 0) {
            return comp_.unsupported(u_, src, "a `for` capture by reference");
        }

        if (slot < binds.size()) { binds[slot] = i; }
    }

    if (is_comptime()) { return run_for(inst, iterables, binds, body, else_body); }
    const auto branch{runtime_branch()};

    auto&                 usize_type{builtin(gs::type_kind::USIZE)};
    auto&                 bool_type{builtin(gs::type_kind::BOOL)};
    auto&                 pool{comp_.constants()};
    std::vector<iterable> infos;

    // Open ranges' known starts, and the length of an array they run alongside
    std::vector<std::pair<usize, big_int>> open_starts;
    stdx::option<usize>                    array_len;
    for (const auto iterable_ref : iterables) {
        const auto iterable_inst{as_inst(iterable_ref)};
        if (iterable_inst && mod().tag_of(*iterable_inst) == tag::range) {
            const auto     bounds{bounds_of(mod(), *iterable_inst)};
            range_operands r{};
            if (bounds.lo) {
                r.lo.emplace(TRY(resolve_value(*bounds.lo, src)));
                const auto lo_inst{as_inst(*bounds.lo)};
                r.lo_is_literal = lo_inst && mod().tag_of(*lo_inst) == tag::int_value;
            }

            if (bounds.hi) { r.hi.emplace(TRY(resolve_value(*bounds.hi, src))); }
            auto&      elem{counter_type(r)};
            const auto convert{[&](const operand& bound) -> result<gir::value> {
                const auto coerced{
                    TRY(coerce_to(bound, elem, src, {.what = use_site::kind::STORE}))};
                return as_runtime(coerced, src);
            }};
            const auto start{r.lo ? TRY(convert(*r.lo)) : usize_constant(0)};
            const auto end{r.hi ? TRY(convert(*r.hi)) : usize_constant(0)};
            const auto slot{builder_.emit_alloca(elem)};
            builder_.emit_store(slot, start);
            if (!r.hi) {
                const auto lo_index{r.lo ? comptime_value(*r.lo) : stdx::none};
                const auto lo{lo_index ? int_of(pool.value_at(*lo_index))
                                       : stdx::option<big_int>{big_int{i128{0}}}};
                if (lo) { open_starts.emplace_back(infos.size(), *lo); }
            }

            stdx::option<gir::value> descending;
            if (r.hi) {
                descending.emplace(
                    builder_.emit_binary(instruction_kind::GT, start, end, bool_type), bool_type);
            }

            infos.emplace_back<iterable>({
                .is_range   = true,
                .inclusive  = bounds.inclusive,
                .open_upper = !r.hi,
                .counter    = gir::value{slot, elem},
                .elem       = &elem,
                .end        = end,
                .descending = descending,
                .container  = {},
                .is_slice   = false,
                .values     = known_range(r, bounds.inclusive),
            });
            continue;
        }

        auto  container{TRY(address_of(iterable_ref, src))};
        auto& container_type{binding_view(iterable_ref, *container.pointee)};
        container.address = gir::value{container.address.data, container_type};
        const auto index_slot{builder_.emit_alloca(usize_type)};
        builder_.emit_store(index_slot, usize_constant(0));
        auto& ctx{comp_.ctx()};
        if (const auto arr{container_type.get_data().as_opt<gs::types::array>()}) {
            array_len.emplace(arr->len);
            infos.emplace_back<iterable>({
                .is_range   = false,
                .inclusive  = false,
                .open_upper = false,
                .counter    = gir::value{index_slot, usize_type},
                .elem       = &*ctx.pool.with_const(arr->underlying, container_type.is_constant()),
                .end        = usize_constant(arr->len),
                .descending = stdx::none,
                .container  = container.address,
                .is_slice   = false,
            });
        } else if (const auto s{container_type.get_data().as_opt<gs::types::slice>()}) {
            const auto len_slot{builder_.emit_get_element_ptr(
                container.address, {usize_constant(SLICE_LEN_FIELD)}, usize_type)};
            const auto len{builder_.emit_load(gir::value{len_slot, usize_type}, usize_type)};
            infos.emplace_back<iterable>({
                .is_range   = false,
                .inclusive  = false,
                .open_upper = false,
                .counter    = gir::value{index_slot, usize_type},
                .elem       = &*ctx.pool.with_const(s->underlying, container_type.is_constant()),
                .end        = gir::value{len, usize_type},
                .descending = stdx::none,
                .container  = container.address,
                .is_slice   = true,
            });
        } else {
            return comp_.unsupported(u_, src, "a `for` over this type");
        }
    }

    // An open range alongside an array takes no more values than the array has
    if (array_len && *array_len > 0) {
        for (const auto& [at, lo] : open_starts) {
            infos[at].values =
                value_range{.lo = lo, .hi = lo + big_int{static_cast<i128>(*array_len - 1)}};
        }
    }

    const auto cond_seg{new_segment()};
    const auto body_seg{new_segment()};
    const auto step_seg{new_segment()};
    const auto else_seg{else_body.empty() ? stdx::none
                                          : stdx::option<gir::segment_id>{new_segment()}};
    const auto exit{new_segment()};
    emit_goto(cond_seg);
    targets_.emplace_back<target>({.inst = inst, .exit = exit, .repeat = step_seg, .arrivals = {}});
    const auto pop{gsl::finally([this] { targets_.pop_back(); })};

    // Each iterable with an end of its own has to stay within it
    switch_to(cond_seg);
    stdx::option<gir::value> cond;
    for (const auto& info : infos) {
        if (info.is_range && (info.open_upper || info.inclusive)) { continue; }
        const auto current{builder_.emit_load(info.counter, *info.counter.type)};
        const auto within{
            builder_.emit_binary(info.is_range ? instruction_kind::NE : instruction_kind::LT,
                                 gir::value{current, *info.counter.type},
                                 info.end,
                                 bool_type)};
        cond = cond ? gir::value{builder_.emit_binary(instruction_kind::AND,
                                                      *cond,
                                                      gir::value{within, bool_type},
                                                      bool_type),
                                 bool_type}
                    : gir::value{within, bool_type};
    }

    const auto done{else_seg.value_or(exit)};
    builder_.emit_cond_goto(
        cond.value_or(constant(pool.intern(true, bool_type), bool_type)), body_seg, done);

    switch_to(body_seg);
    for (usize i{0}; i < infos.size(); ++i) {
        if (!binds[i]) { continue; }
        const auto& info{infos[i]};
        auto&       read_only{*comp_.ctx().pool.with_const(*info.elem, true)};
        if (info.is_range) {
            // The counter changes in the step, so the body reads a snapshot of it
            const auto current{builder_.emit_load(info.counter, *info.elem)};
            const auto snapshot{builder_.emit_alloca(*info.elem)};
            builder_.emit_store(snapshot, gir::value{current, *info.elem}).is_initializer = true;
            values_.insert_or_assign(*binds[i],
                                     slot_value{.address    = gir::value{snapshot, read_only},
                                                .type       = &read_only,
                                                .is_mutable = false,
                                                .range      = info.values});
            continue;
        }

        const auto index{builder_.emit_load(info.counter, usize_type)};
        gir::value base{info.container};
        if (info.is_slice) {
            const auto& s{info.container.type->get_data().as<gs::types::slice>()};
            auto&       ptr_type{comp_.ctx().get_pointer(info.container.type->is_constant()
                                                             ? gs::types::mut::CONSTANT
                                                             : gs::types::mut::MUTABLE,
                                                         s.underlying)};
            const auto  ptr_slot{builder_.emit_get_element_ptr(
                info.container, {usize_constant(SLICE_PTR_FIELD)}, ptr_type)};
            base =
                gir::value{builder_.emit_load(gir::value{ptr_slot, ptr_type}, ptr_type), ptr_type};
        }

        const auto at{
            builder_.emit_get_element_ptr(base, {gir::value{index, usize_type}}, read_only)};
        values_.insert_or_assign(
            *binds[i], slot_value{.address = gir::value{at, read_only}, .type = &read_only});
    }

    DISCARD(TRY(analyze_body(body)));
    if (!terminated()) { emit_goto(step_seg); }

    switch_to(step_seg);
    for (const auto& info : infos) {
        if (!info.is_range) {
            const auto current{builder_.emit_load(info.counter, usize_type)};
            const auto next{builder_.emit_binary(instruction_kind::ADD,
                                                 gir::value{current, usize_type},
                                                 usize_constant(1),
                                                 usize_type)};
            builder_.emit_store(info.counter, gir::value{next, usize_type});
            continue;
        }

        auto&      elem{*info.elem};
        const auto current{builder_.emit_load(info.counter, elem)};

        // `lo..=hi` ends on `hi` itself, which may be its type's largest value
        if (info.inclusive) {
            const auto at_end{builder_.emit_binary(
                instruction_kind::EQ, gir::value{current, elem}, info.end, bool_type)};
            const auto step_on{new_segment()};
            builder_.emit_cond_goto(gir::value{at_end, bool_type}, done, step_on);
            switch_to(step_on);
        }

        const auto one{constant(pool.intern(i64{1}, elem), elem)};
        const auto advance{[&](instruction_kind kind) {
            const auto next{builder_.emit_binary(kind, gir::value{current, elem}, one, elem)};
            builder_.emit_store(info.counter, gir::value{next, elem});
        }};
        if (!info.descending) {
            advance(instruction_kind::ADD);
            continue;
        }

        const auto down{new_segment()};
        const auto up{new_segment()};
        const auto stepped{new_segment()};
        builder_.emit_cond_goto(*info.descending, down, up);
        switch_to(down);
        advance(instruction_kind::SUB);
        emit_goto(stepped);
        switch_to(up);
        advance(instruction_kind::ADD);
        emit_goto(stepped);
        switch_to(stepped);
    }

    if (!terminated()) { emit_goto(cond_seg); }

    stdx::option<incoming> normal;
    if (else_seg) {
        switch_to(*else_seg);
        const auto out{TRY(analyze_body(else_body))};
        if (out.yielded && !terminated()) {
            normal.emplace(builder_.get_segment()->get_id(), *out.yielded, src);
            emit_goto(exit);
        }
    }

    auto arrivals{targets_.back().arrivals};
    if (normal) { arrivals.emplace_back<incoming>(std::move(*normal)); }
    if (arrivals.empty() && !else_seg) {
        switch_to(exit);
        return void_value();
    }

    return finish_merge(arrivals, exit, src);
}

} // namespace ghoti::uir::sema
