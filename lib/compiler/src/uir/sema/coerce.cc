#include <utility>
#include <vector>

#include <gsl/pointers>
#include <gsl/span>
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

auto analyzer::coerce(const operand& op, gs::type& target, const source& src)
    -> result<stdx::option<operand>> {
    if (const auto m{op.as_opt<merge_ref>()}) {
        if (!gs::is_numeric(target.get_kind())) { return stdx::none; }
        return operand{TRY(materialize_merge(*m, target, src))};
    }
    auto& from{type_of(op)};
    if (is_poison(from) || is_poison(target)) { return stdx::err{stop::POISON}; }
    if (gs::is_same_unqualified(from, target)) { return op; }

    auto& pool{comp_.constants()};
    auto& ctx{comp_.ctx()};
    if (const auto index{comptime_value(op)}) {
        if (pool.is_type(*index)) { return stdx::none; }
        const auto& v{pool.value_at(*index)};
        if (from.get_kind() == gs::type_kind::UNDEFINED) {
            return operand{pool.intern(ip::undefined_value{}, target)};
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
        // A known integer converts to any float, as the old emitter's `int_as_float` does
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
        return stdx::none;
    }

    if (const auto value{op.as_opt<gir::value>()}) {
        if (gs::is_numeric(from.get_kind()) && gs::is_numeric(target.get_kind()) &&
            gs::is_implicit_widenable(from, target)) {
            TRY(require_runtime(src));
            return operand{gir::value{
                builder_.emit_cast(gir::instruction_kind::WIDEN_CAST, *value, target), target}};
        }
    }
    return stdx::none;
}

auto analyzer::coerce_to(const operand&  op,
                         gs::type&       target,
                         const source&   src,
                         const use_site& site) -> result<operand> {
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
    if (const auto m{op.as_opt<merge_ref>()}) {
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
    if (const auto index{comptime_value(typed)}) {
        if (comp_.constants().is_type(*index)) {
            return comp_.unsupported(u_, src, "a type used as a runtime value");
        }
        return constant(*index, type_of(typed));
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
    for (const auto& arrival : pending.arrivals) {
        const auto stored{
            TRY(coerce_to(arrival.value, type, arrival.src, {.what = use_site::kind::STORE}))};
        const auto value{TRY(as_runtime(stored, arrival.src))};
        builder_.set_segment(*gir_fn_->get_segment(arrival.seg));
        builder_.set_splice(true);
        builder_.emit_store(slot, value);
        builder_.set_splice(false);
    }
    builder_.set_segment(*restore);
    builder_.set_splice(was_splicing);
    const gir::value loaded{builder_.emit_load(slot, type), type};
    if (builder_.get_segment()->get_id() == pending.seg) { pending.materialized.emplace(loaded); }
    return loaded;
}

} // namespace ghoti::uir::sema
