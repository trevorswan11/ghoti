#include <utility>
#include <vector>

#include <gsl/pointers>
#include <gsl/span>
#include <gsl/util>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/gir/semantics.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/peer_type.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/big_int.hh"
#include "support/float128.hh"
#include "support/int128.hh"

namespace ghoti::uir::sema {

namespace {

[[nodiscard]] auto is_poison(const gs::type& t) noexcept -> bool { return t.is_poison(); }

} // namespace

auto int_of(const ip::value& v) -> stdx::option<big_int> {
    return v.data.visit(
        [](i64 x) -> stdx::option<big_int> { return big_int{static_cast<i128>(x)}; },
        [](u64 x) -> stdx::option<big_int> { return big_int{static_cast<u128>(x)}; },
        [](i128 x) -> stdx::option<big_int> { return big_int{x}; },
        [](u128 x) -> stdx::option<big_int> { return big_int{x}; },
        [](const big_int& x) -> stdx::option<big_int> { return x; },
        [](const auto&) -> stdx::option<big_int> { return stdx::none; });
}

auto decays_to(const gs::type& from, const gs::type& to) -> bool {
    const auto from_array{from.get_data().as_opt<gs::types::array>()};
    const auto to_array{to.get_data().as_opt<gs::types::array>()};
    if (!from_array || !to_array || from_array->len != to_array->len) { return false; }
    const auto ref{from_array->underlying.get_data().as_opt<gs::types::reference>()};
    const auto ptr{to_array->underlying.get_data().as_opt<gs::types::pointer>()};
    if (!ref || !ptr) { return false; }

    // A pointer may not grant a write its reference doesn't
    const bool permitted{!from_array->underlying.is_constant() ||
                         to_array->underlying.is_constant()};
    return permitted && gs::is_same_unqualified(ref->underlying, ptr->underlying);
}

auto analyzer::coerce(const operand& op, gs::type& target, const source& src)
    -> result<stdx::option<operand>> {
    if (const auto m{op.as_opt<merge_ref>()}) {
        const auto kind{target.get_kind()};
        const bool literals{is_untyped_literal(merges_[m->index].arrivals.front().value)};
        const bool takes{literals ? kind == gs::type_kind::STRUCT || kind == gs::type_kind::UNION ||
                                        kind == gs::type_kind::ARRAY || kind == gs::type_kind::ENUM
                                  : gs::is_numeric(kind)};
        if (!takes) { return stdx::none; }
        return operand{TRY(materialize_merge(*m, target, src))};
    }

    if (const auto literal{op.as_opt<enum_literal>()}) {
        if (target.get_kind() != gs::type_kind::ENUM) { return stdx::none; }
        return enum_constant(target, mod().string(mod().data_of(literal->inst).str), src);
    }

    // A cast's target is the type it is used as, unless that has no width of its own
    if (const auto cast{op.as_opt<cast_ref>()}) {
        if (target.get_kind() == gs::type_kind::AUTO ||
            gs::is_comptime_numeric(target.get_kind())) {
            return operand{TRY(cast_target_inferred(*cast, stdx::none))};
        }

        return operand{TRY(cast_target_inferred(*cast, target))};
    }

    if (const auto init{op.as_opt<anon_init>()}) {
        const auto kind{target.get_kind()};
        if (kind != gs::type_kind::STRUCT && kind != gs::type_kind::UNION &&
            kind != gs::type_kind::ARRAY) {
            return stdx::none;
        }

        return operand{TRY(init_as(init->inst, target, src))};
    }

    auto& from{type_of(op)};
    if (is_poison(from) || is_poison(target)) { return stdx::err{stop::POISON}; }
    if (gs::is_same_unqualified(from, target)) { return op; }
    if (auto fat{TRY(coerce_to_dyn(op, target, src))}) { return std::move(*fat); }

    // An array of references is an array of pointers to the same values
    if (const auto value{op.as_opt<gir::value>()}; value && decays_to(from, target)) {
        return operand{gir::value{value->data, target}};
    }

    auto& pool{comp_.constants()};
    auto& ctx{comp_.ctx()};

    // A range capture converts to any integer that holds every value it takes
    if (const auto slot{op.as_opt<slot_value>()};
        slot && slot->range && gs::is_integer(target.get_kind()) &&
        gs::comptime_int_fits(slot->range->lo, target, comp_.ptr_bits()) &&
        gs::comptime_int_fits(slot->range->hi, target, comp_.ptr_bits())) {
        TRY(require_runtime(src));
        const auto value{
            load(place{.address = slot->address, .pointee = slot->type, .is_mutable = false})};
        return operand{
            gir::value{builder_.emit_cast(gir::instruction_kind::INT_CAST, value, target), target}};
    }

    if (from.get_kind() == gs::type_kind::ARRAY && target.get_kind() == gs::type_kind::SLICE) {
        if (const auto known{comptime_value(op)}; known && is_comptime()) {
            return comptime_slice_of(*known, from, target);
        }

        if (!comptime_value(op)) {
            const auto base{TRY(operand_address(op, src))};
            return array_as_slice(base.address, from, target);
        }
    }

    // A reference to an array views it as a slice
    if (from.get_kind() == gs::type_kind::REFERENCE && target.get_kind() == gs::type_kind::SLICE) {
        auto& referent{from.get_data().as<gs::types::reference>().underlying};
        if (referent.get_kind() == gs::type_kind::ARRAY &&
            (target.is_constant() || !from.is_constant())) {
            TRY(require_runtime(src));
            return array_as_slice(TRY(as_runtime(op, src)), referent, target);
        }
    }

    if (const auto index{comptime_value(op)}) {
        if (pool.is_type(*index)) { return stdx::none; }
        const auto& v{pool.value_at(*index)};
        if (from.get_kind() == gs::type_kind::UNDEFINED) {
            return operand{pool.intern(ip::undefined_value{}, target)};
        }

        if (v.data.is<ip::nullptr_value>() && target.get_kind() == gs::type_kind::POINTER) {
            return operand{pool.intern(ip::nullptr_value{}, target)};
        }

        // A known typed integer is exactly a `comptime_int`
        if (const auto value{int_of(v)}; value &&
                                         target.get_kind() == gs::type_kind::COMPTIME_INT &&
                                         gs::is_integer(from.get_kind())) {
            return operand{intern_int(pool, *value, target)};
        }

        if (const auto value{int_of(v)}; value && gs::is_integer(target.get_kind())) {
            if (!gs::comptime_int_fits(*value, target, comp_.ptr_bits())) {
                return comp_.report(u_,
                                    src,
                                    gs::error::LITERAL_OUT_OF_RANGE,
                                    from.get_kind() == gs::type_kind::COMPTIME_INT
                                        ? messages::literal_out_of_range(ctx, target)
                                        : messages::value_out_of_range(ctx, *value, target));
            }

            return operand{intern_int(pool, *value, target)};
        }

        // A known integer converts to any float
        const bool to_float{gs::is_float(target.get_kind()) ||
                            target.get_kind() == gs::type_kind::COMPTIME_FLOAT};
        if (to_float && int_of(v)) {
            const auto value{*int_of(v)};
            const auto exact{value.to_i128()};
            if (!exact) {
                return comp_.unsupported(u_, src, "a wide integer converted to a float");
            }

            const auto as_float{gir::semantics::float_from_int(
                {.bits = 128, .is_signed = true}, static_cast<u128>(*exact), float_format::QUAD)};
            return operand{gir::value::constant(pool, as_float, target).constant_index()};
        }

        if (const auto f{v.data.as_opt<f128>()};
            f && gs::is_float(target.get_kind()) &&
            (from.get_kind() == gs::type_kind::COMPTIME_FLOAT ||
             gs::is_implicit_widenable(from, target))) {
            return operand{gir::value::constant(pool, *f, target).constant_index()};
        }

        // A string's bytes view as a byte slice, a sentinel one only from a sentinel string
        if (const auto text{v.data.as_opt<std::string>()};
            text && target.get_kind() == gs::type_kind::SLICE) {
            const auto& slice{target.get_data().as<gs::types::slice>()};
            const auto  arr{from.get_data().as_opt<gs::types::array>()};
            const auto  bytes{gs::as_integer(slice.underlying)};
            if (bytes && bytes->bits == 8 && !bytes->is_signed &&
                (!slice.null_terminated || (arr && arr->null_terminated))) {
                return operand{pool.intern(*text, target)};
            }
        }

        return stdx::none;
    }

    if (const auto value{op.as_opt<gir::value>()}) {
        if (gs::is_numeric(from.get_kind()) && gs::is_numeric(target.get_kind()) &&
            gs::is_implicit_widenable(from, target)) {
            TRY(require_runtime(src));
            return operand{gir::value{
                builder_.emit_cast(gir::instruction_kind::WIDEN_CAST, *value, target), target}};
        }

        // A view that only loses mutability is the same value
        const auto is_view{[](const gs::type& t) {
            const auto kind{t.get_kind()};
            return kind == gs::type_kind::POINTER || kind == gs::type_kind::REFERENCE ||
                   kind == gs::type_kind::SLICE;
        }};
        if (is_view(from) && from.get_kind() == target.get_kind() &&
            gs::is_assignable(from, target)) {
            return op;
        }
    }

    return stdx::none;
}

auto analyzer::array_as_slice(const gir::value& address, gs::type& from, gs::type& target)
    -> result<stdx::option<operand>> {
    const auto& arr{from.get_data().as<gs::types::array>()};
    const auto& slice{target.get_data().as<gs::types::slice>()};
    if (!gs::is_same_unqualified(arr.underlying, slice.underlying) ||
        (slice.null_terminated && !arr.null_terminated) ||
        (!target.is_constant() && from.is_constant())) {
        return stdx::none;
    }

    return operand{TRY(slice_of_array(address, from))};
}

auto analyzer::comptime_slice_of(ip::index array, gs::type& from, gs::type& target)
    -> stdx::option<operand> {
    const auto& arr{from.get_data().as<gs::types::array>()};
    const auto& slice{target.get_data().as<gs::types::slice>()};
    if (!gs::is_same_unqualified(arr.underlying, slice.underlying) ||
        (slice.null_terminated && !arr.null_terminated) ||
        (!target.is_constant() && from.is_constant())) {
        return stdx::none;
    }

    // A compile-time slice holds its elements, as the array did
    auto&       pool{comp_.constants()};
    const auto& data{pool.value_at(array).data};
    if (const auto agg{data.as_opt<ip::aggregate_value>()}) {
        return operand{pool.intern(ip::aggregate_value{*agg}, target)};
    }

    if (const auto text{data.as_opt<std::string>()}) {
        return operand{pool.intern(std::string{*text}, target)};
    }

    return stdx::none;
}

auto analyzer::slice_of_array(const gir::value& address, gs::type& from) -> result<gir::value> {
    const auto& arr{from.get_data().as<gs::types::array>()};
    auto&       ctx{comp_.ctx()};
    const auto  mut{from.is_constant() ? gs::types::mut::CONSTANT : gs::types::mut::MUTABLE};
    auto&       ptr_type{ctx.get_pointer(mut, arr.underlying)};
    auto&       slice_type{ctx.get_slice(mut, arr.null_terminated, arr.underlying)};
    auto&       usize_type{builtin(gs::type_kind::USIZE)};
    const auto  slot{builder_.emit_alloca(slice_type)};
    const auto  elem0{builder_.emit_get_element_ptr(address, {usize_constant(0)}, ptr_type)};
    const auto  ptr_field{builder_.emit_get_element_ptr(
        gir::value{slot, slice_type}, {usize_constant(SLICE_PTR_FIELD)}, ptr_type)};
    builder_.emit_store(gir::value{ptr_field, ptr_type}, gir::value{elem0, ptr_type})
        .is_initializer = true;
    const auto len_field{builder_.emit_get_element_ptr(
        gir::value{slot, slice_type}, {usize_constant(SLICE_LEN_FIELD)}, usize_type)};
    builder_.emit_store(gir::value{len_field, usize_type}, usize_constant(arr.len)).is_initializer =
        true;
    return gir::value{builder_.emit_load(gir::value{slot, slice_type}, slice_type), slice_type};
}

auto analyzer::coerce_to(const operand&  op,
                         gs::type&       target,
                         const source&   src,
                         const use_site& site) -> result<operand> {
    // A runtime integer converted to another integer type folds when it is known at compile time
    if (const auto value{op.as_opt<gir::value>()};
        value && site.from && !is_comptime() && value->type &&
        gs::is_integer(value->type->get_kind()) && gs::is_integer(target.get_kind()) &&
        !gs::is_same_unqualified(*value->type, target)) {
        if (const auto known{TRY(speculated_value(*site.from, src))}) {
            if (auto coerced{TRY(coerce(operand{*known}, target, src))}) {
                return std::move(*coerced);
            }
        }
    }

    if (auto coerced{TRY(coerce(op, target, src))}) { return std::move(*coerced); }
    auto&       ctx{comp_.ctx()};
    const auto& from{type_of(op)};
    switch (site.what) {
    case use_site::kind::STORE:
        return comp_.report(u_,
                            src,
                            gs::error::TYPE_MISMATCH,
                            messages::store_mismatch(ctx, from, target, comp_.ptr_bits()));
    case use_site::kind::RETURN:
        return comp_.report(u_,
                            src,
                            gs::error::RETURN_TYPE_MISMATCH,
                            messages::return_mismatch(ctx, from, target, comp_.ptr_bits()));
    case use_site::kind::ARGUMENT:
        return comp_.report(u_,
                            src,
                            gs::error::TYPE_MISMATCH,
                            messages::argument_mismatch(
                                ctx, site.arg_index, from, target, site.callee, comp_.ptr_bits()));
    }

    UNREACHABLE("every use site has a message");
}

auto analyzer::peer_type(gsl::span<const operand> ops, const source& src)
    -> result<gsl::not_null<gs::type*>> {
    std::vector<gs::peer_operand> peers;
    peers.reserve(ops.size());
    for (const auto& op : ops) {
        auto& t{type_of(op)};
        if (t.is_poison()) { return stdx::err{stop::POISON}; }
        peers.emplace_back<gs::peer_operand>({.type = &t});
    }

    auto&      ctx{comp_.ctx()};
    const auto peer{gs::peer_type(ctx, peers)};
    if (!peer) {
        return comp_.report(
            u_, src, gs::error::NO_PEER_TYPE, gs::peer_error_message(ctx, peers, peer.error()));
    }

    return *peer;
}

auto analyzer::concrete(const operand& op, const source& src) -> result<operand> {
    if (const auto cast{op.as_opt<cast_ref>()}) { return cast_target_inferred(*cast, stdx::none); }
    if (const auto m{op.as_opt<merge_ref>()}) {
        if (is_untyped_literal(merges_[m->index].arrivals.front().value)) {
            return comp_.unsupported(u_, src, "an untyped literal with no type to take");
        }

        auto& untyped{type_of(op)};
        return operand{TRY(materialize_merge(*m, comp_.ctx().default_concrete(untyped), src))};
    }

    auto& t{type_of(op)};
    if (comptime_value(op) && gs::is_comptime_numeric(t.get_kind())) {
        auto& target{comp_.ctx().default_concrete(t)};
        if (auto coerced{TRY(coerce(op, target, src))}) { return std::move(*coerced); }
    }

    return op;
}

auto analyzer::as_runtime(const operand& op, const source& src) -> result<gir::value> {
    const auto typed{TRY(concrete(op, src))};
    if (const auto value{typed.as_opt<gir::value>()}) { return *value; }
    if (const auto slot{typed.as_opt<slot_value>()}) {
        TRY(require_runtime(src));
        if (slot->origin) { return *slot->origin; }
        return load(place{.address = slot->address, .pointee = slot->type, .is_mutable = false});
    }

    if (const auto global{typed.as_opt<global_const>()}) {
        TRY(require_runtime(src));
        return load(global_place(*global->symbol, *global->type, false));
    }

    if (const auto index{comptime_value(typed)}) {
        if (comp_.constants().is_type(*index)) {
            return comp_.unsupported(u_, src, "a type used as a runtime value");
        }

        const auto& data{comp_.constants().value_at(*index).data};
        if (data.is<ip::aggregate_value>() || data.is<ip::union_value>()) {
            TRY(require_runtime(src));
            return materialize(*index, src);
        }

        if (const auto text{data.as_opt<std::string>()};
            text && type_of(typed).get_kind() == gs::type_kind::SLICE) {
            TRY(require_runtime(src));
            return string_as_slice(*text, type_of(typed));
        }

        return constant(*index, type_of(typed));
    }

    if (typed.is<anon_init>() || typed.is<enum_literal>()) {
        return comp_.unsupported(u_, src, "an untyped literal with no type to take");
    }

    return comp_.unsupported(u_, src, "a function or place used as a runtime value");
}

auto analyzer::materialize_merge(merge_ref m, gs::type& type, const source& src)
    -> result<gir::value> {
    auto&      pending{merges_[m.index]};
    const auto here{builder_.get_segment()};
    if (pending.materialized && pending.materialized->type &&
        gs::is_same_unqualified(*pending.materialized->type, type) && here &&
        here->get_id() == pending.seg) {
        return *pending.materialized;
    }

    TRY(require_runtime(src));

    const auto slot{builder_.emit_alloca_in_entry(type)};
    const auto restore{builder_.get_segment()};
    const bool was_splicing{builder_.is_splicing()};
    {
        const auto back{gsl::finally([&] {
            builder_.set_segment(*restore);
            builder_.set_splice(was_splicing);
        })};

        // Each arm converts where it arrives, which an initializer needs room in
        for (const auto& arrival : pending.arrivals) {
            builder_.set_segment(*gir_fn_->get_segment(arrival.seg));
            builder_.set_splice(true);
            const auto stored{
                TRY(coerce_to(arrival.value, type, arrival.src, {.what = use_site::kind::STORE}))};
            const auto value{TRY(as_runtime(stored, arrival.src))};
            builder_.emit_store(slot, value);
        }
    }

    const gir::value loaded{builder_.emit_load(slot, type), type};
    if (builder_.get_segment()->get_id() == pending.seg) { pending.materialized.emplace(loaded); }
    return loaded;
}

} // namespace ghoti::uir::sema
