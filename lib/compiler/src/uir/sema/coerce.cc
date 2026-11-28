#include <algorithm>
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
#include "support/string_utils.hh"

namespace ghoti::uir::sema {

namespace {

[[nodiscard]] auto is_poison(const gs::type& t) noexcept -> bool { return t.is_poison(); }

// Whether a view of `target` grants no write through it that `from` doesn't, at every level
[[nodiscard]] auto writes_permitted(const gs::type& from, const gs::type& target) -> bool {
    const auto viewed{[](const gs::type& t) -> stdx::option<const gs::type&> {
        if (const auto p{t.get_data().as_opt<gs::types::pointer>()}) { return p->underlying; }
        if (const auto r{t.get_data().as_opt<gs::types::reference>()}) { return r->underlying; }
        if (const auto s{t.get_data().as_opt<gs::types::slice>()}) { return s->underlying; }
        return stdx::none;
    }};
    const auto from_inner{viewed(from)};
    const auto target_inner{viewed(target)};
    if (!from_inner || !target_inner) { return true; }
    // `mut?` takes either mutability, so it settles nothing here
    if (!from.is_poly() && !target.is_poly() && from.is_constant() && !target.is_constant()) {
        return false;
    }
    return writes_permitted(*from_inner, *target_inner);
}

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
        const auto  kind{target.get_kind()};
        const auto& arrivals{merges_[m->index].arrivals};
        const bool  literals{merge_holds_literals(*m)};
        // An inferred cast takes whatever type it meets
        const bool casts{std::ranges::any_of(
            arrivals, [](const incoming& a) { return a.value.is<cast_ref>(); })};
        const bool takes{
            casts || (literals ? kind == gs::type_kind::STRUCT || kind == gs::type_kind::UNION ||
                                     kind == gs::type_kind::ARRAY || kind == gs::type_kind::ENUM
                               : gs::is_numeric(kind))};
        if (!takes) { return stdx::none; }
        return operand{TRY(materialize_merge(*m, target, src))};
    }

    if (const auto call{op.as_opt<implicit_call>()}) {
        const auto kind{target.get_kind()};
        if (kind != gs::type_kind::STRUCT && kind != gs::type_kind::UNION &&
            kind != gs::type_kind::ENUM) {
            return stdx::none;
        }

        auto       rec{record(call->inst)};
        const auto literal{as_inst(*rec.ref_field())};
        rec.skip_ref();
        const auto args{rec.refs_field()};
        const auto name{mod().string(mod().data_of(*literal).str)};
        const auto member{TRY(member_of(target, name, src))};
        const auto result{TRY(call_operand(member, args, src, src))};
        return operand{TRY(coerce_to(result, target, src, {.what = use_site::kind::STORE}))};
    }

    if (const auto literal{op.as_opt<enum_literal>()}) {
        // `.name` of a struct or union is its member constant, unless it names a union's field
        const auto name{literal_name(*literal)};
        const auto kind{target.get_kind()};
        if (kind == gs::type_kind::ENUM_LITERAL) { return op; }

        // `.name` as a function is the member of the type it returns
        const auto pointee{gs::pointee_of(target)};
        const auto signature{
            (pointee ? *pointee : target).get_data().as_opt<gs::types::function>()};
        if (const auto returned{signature ? stdx::option<gs::type&>{signature->return_type}
                                          : stdx::none};
            returned && (returned->get_kind() == gs::type_kind::STRUCT ||
                         returned->get_kind() == gs::type_kind::UNION ||
                         returned->get_kind() == gs::type_kind::ENUM)) {
            TRY(comp_.require_fields(u_, src, *returned));
            const auto member{TRY(member_of(*returned, name, src))};
            return operand{TRY(coerce_to(member, target, src, {.what = use_site::kind::STORE}))};
        }

        if (kind == gs::type_kind::STRUCT || kind == gs::type_kind::UNION) {
            TRY(comp_.require_fields(u_, src, target));
        }

        const auto union_field{[&] {
            const auto ut{target.get_data().as_opt<gs::types::union_t>()};
            return ut && std::ranges::contains(ut->field_names, name);
        }};
        if ((kind == gs::type_kind::STRUCT || kind == gs::type_kind::UNION) && !union_field()) {
            auto member{TRY(member_of(target, name, src))};

            // Compile time reads a constant's value rather than its global
            if (const auto g{member.as_opt<global_const>()}; g && is_comptime()) {
                member = operand{g->value};
            }

            return operand{TRY(coerce_to(member, target, src, {.what = use_site::kind::STORE}))};
        }

        // A union's `void` field holds nothing to give it
        if (kind == gs::type_kind::UNION) {
            const auto& ut{target.get_data().as<gs::types::union_t>()};
            const auto  at{std::ranges::find(ut.field_names, name)};
            const usize index{static_cast<usize>(at - ut.field_names.begin())};
            if (ut.type_at(index).get_kind() != gs::type_kind::VOID_) { return stdx::none; }
            return operand{TRY(union_of(target, index, void_value(), src))};
        }

        // A type with no members has nothing for `.name` to name
        if (gs::is_numeric(kind) || kind == gs::type_kind::BOOL) {
            return comp_.report(u_,
                                src,
                                gs::error::TYPE_MISMATCH,
                                fmt::format("Can only access inner objects inside of structs, "
                                            "unions, and enums; found '{}'",
                                            comp_.ctx().type_display_name(target)));
        }

        if (target.get_kind() != gs::type_kind::ENUM) { return stdx::none; }
        return enum_constant(target, name, src);
    }

    // A cast's target is the type it is used as, unless that has no width of its own
    if (const auto cast{op.as_opt<cast_ref>()}) {
        if (target.get_kind() == gs::type_kind::AUTO ||
            gs::is_comptime_numeric(target.get_kind())) {
            return operand{TRY(cast_target_inferred(*cast, stdx::none))};
        }

        return operand{TRY(cast_target_inferred(*cast, target))};
    }

    if (const auto lit{op.as_opt<slice_literal>()}) {
        if (target.get_kind() != gs::type_kind::SLICE) { return stdx::none; }

        // The sugar only makes a slice of constant elements
        if (!target.is_constant()) {
            return comp_.report(u_,
                                src,
                                gs::error::TYPE_MISMATCH,
                                fmt::format("Only struct and union types may be used in "
                                            "initializer expressions; found '{}'",
                                            comp_.ctx().type_display_name(target)));
        }
        return slice_literal_as(*lit, target, src);
    }

    if (const auto init{op.as_opt<anon_init>()}) {
        const auto kind{target.get_kind()};

        // A range value is the array its values fill, which a slice views
        if (mod().tag_of(init->inst) == tag::range) {
            const auto arr{target.get_data().as_opt<gs::types::array>()};
            const auto s{target.get_data().as_opt<gs::types::slice>()};
            if (!arr && !s) { return stdx::none; }
            auto&      elem{arr ? arr->underlying : s->underlying};
            const auto value{TRY(range_value(init->inst, elem, src))};
            if (arr) {
                const auto len{comp_.constants()
                                   .value_at(*comptime_value(value))
                                   .data.as<ip::aggregate_value>()
                                   .elements.size()};
                if (len != arr->len) {
                    return comp_.report(u_,
                                        src,
                                        gs::error::TYPE_MISMATCH,
                                        fmt::format("Expected {} array element{}, found {}",
                                                    arr->len,
                                                    string_utils::plural_suffix(arr->len),
                                                    len));
                }

                return operand{comp_.constants().intern(
                    comp_.constants().value_at(*comptime_value(value)).data, target)};
            }

            return coerce_to(value, target, src, {.what = use_site::kind::STORE});
        }

        if (kind == gs::type_kind::VECTOR) { return coerce_vector(op, target, target, src); }
        if (kind != gs::type_kind::STRUCT && kind != gs::type_kind::UNION &&
            kind != gs::type_kind::ARRAY) {
            return stdx::none;
        }

        return operand{TRY(init_as(init->inst, target, src))};
    }

    auto& from{type_of(op)};

    // A compile-time reference used as a value is what it refers to
    if (from.get_kind() == gs::type_kind::REFERENCE &&
        target.get_kind() != gs::type_kind::REFERENCE &&
        target.get_kind() != gs::type_kind::POINTER) {
        if (const auto referent{comptime_referent(op)}) {
            return operand{
                TRY(coerce_to(operand{*referent}, target, src, {.what = use_site::kind::STORE}))};
        }
    }

    // A known address of an untyped value points at that value in the target's pointee type
    if (const auto from_pointee{gs::pointee_of(from)}, to_pointee{gs::pointee_of(target)};
        from_pointee && to_pointee && from.get_kind() == target.get_kind() &&
        gs::is_comptime_numeric(from_pointee->get_kind())) {
        if (const auto known{comptime_value(op)}) {
            auto&      pool{comp_.constants()};
            const auto address{pool.value_at(*known).data.as_opt<ip::address_value>()};
            if (address && address->pointee.size() == 1 && address->symbol.empty()) {
                const auto pointee{address->pointee.front()};
                const auto converted{TRY(coerce_to(
                    operand{pointee}, *to_pointee, src, {.what = use_site::kind::STORE}))};
                const auto typed{TRY(require_comptime(converted, src))};
                return operand{
                    pool.intern(ip::address_value{.symbol = {}, .pointee = {typed}}, target)};
            }
        }
    }
    if (is_poison(from) || is_poison(target)) { return stdx::err{stop::POISON}; }
    if (gs::is_same_unqualified(from, target)) {
        if (!writes_permitted(from, target)) { return stdx::option<operand>{}; }
        return op;
    }
    if (auto fat{TRY(coerce_to_dyn(op, target, src))}) { return std::move(*fat); }
    if (from.get_kind() == gs::type_kind::ENUM && target.get_kind() == gs::type_kind::ENUM &&
        gs::is_enum_widenable(from, target)) {
        return operand{TRY(widen_enum(op, target, src))};
    }

    // An array of references is an array of pointers to the same values
    if (const auto value{op.as_opt<gir::value>()}; value && decays_to(from, target)) {
        return operand{gir::value{value->data, target}};
    }

    auto& pool{comp_.constants()};
    auto& ctx{comp_.ctx()};

    // A union tagged by an enum reads as the variant its active field names
    if (const auto ut{from.get_data().as_opt<gs::types::union_t>()};
        ut && ut->tag_enum && gs::is_same_unqualified(*ut->tag_enum, target)) {
        return operand{TRY(union_tag_as_enum(op, from, target, src))};
    }

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
        // A mutable slice would alias the array, which a compile-time slice value can't
        if (const auto known{comptime_value(op)}; known && is_comptime()) {
            if (!target.is_constant()) {
                if (gir_fn_) { return comp_.needs_runtime(u_, src); }
                return comp_.unsupported(u_, src, "a mutable slice of a compile-time array");
            }

            return comptime_slice_of(*known, from, target);
        }

        if (!comptime_value(op)) {
            // A global just read is sliced where it lives, not through a copy
            const auto v{op.as_opt<gir::value>()};
            const bool bound{v && std::ranges::any_of(values_, [&](const auto& entry) {
                                 const auto held{entry.second.template as_opt<gir::value>()};
                                 const auto t{mod().tag_of(entry.first)};
                                 return held && *held == *v && (is_binding(t) || t == tag::load);
                             })};
            if (const auto stored{v && !bound ? unload(*builder_.get_segment(), *v, from, true)
                                              : stdx::none}) {
                TRY(require_runtime(src));
                return array_as_slice(*stored, from, target);
            }

            const auto base{TRY(operand_address(op, src))};
            return array_as_slice(base.address, from, target);
        }

        // A known array a runtime body slices is built, then copied into a slot of its own
        if (const auto known{comptime_value(op)};
            !comp_.constants().is_type(*known) &&
            comp_.constants().value_at(*known).data.is<ip::aggregate_value>()) {
            const auto built{TRY(as_runtime(op, src))};
            const auto slot{spill(built, from, false)};
            return array_as_slice(slot.address, from, target);
        }
    }

    // A runtime sentinel slice is a slice of the same elements, laid out the same
    if (const auto from_slice{from.get_data().as_opt<gs::types::slice>()};
        from_slice && from_slice->null_terminated && !comptime_value(op) &&
        target.get_kind() == gs::type_kind::SLICE) {
        const auto& to_slice{target.get_data().as<gs::types::slice>()};
        if (!to_slice.null_terminated &&
            gs::is_same_unqualified(from_slice->underlying, to_slice.underlying) &&
            (target.is_constant() || !from.is_constant())) {
            // The value keeps its sentinel type, as the old emitter passes it
            TRY(require_runtime(src));
            return operand{TRY(as_runtime(op, src))};
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
            // A null `^fn` is both of its words zeroed
            if (gs::is_fat_callable(target)) {
                return operand{pool.intern(ip::zero_value{}, target)};
            }
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
            const auto format{gs::float_format_of(target).value_or(float_format::QUAD)};
            const auto as_float{float_of_int(*int_of(v), format)};
            if (as_float.is_infinite() && from.get_kind() == gs::type_kind::COMPTIME_INT) {
                return comp_.report(u_,
                                    src,
                                    gs::error::LITERAL_OUT_OF_RANGE,
                                    fmt::format("literal is out of range for type '{}'",
                                                ctx.type_display_name(target)));
            }
            return operand{gir::value::constant(pool, as_float, target).constant_index()};
        }

        if (const auto f{v.data.as_opt<f128>()};
            f && gs::is_float(target.get_kind()) &&
            (from.get_kind() == gs::type_kind::COMPTIME_FLOAT ||
             gs::is_implicit_widenable(from, target))) {
            // A finite value that rounds to infinity has no value in the target, while a pole
            // is already infinite
            if (const auto format{gs::float_format_of(target)};
                format && from.get_kind() == gs::type_kind::COMPTIME_FLOAT && f->is_finite() &&
                f->round_to(*format).is_infinite()) {
                return comp_.report(u_,
                                    src,
                                    gs::error::LITERAL_OUT_OF_RANGE,
                                    fmt::format("float value {} is out of range for type '{}'",
                                                *f,
                                                ctx.type_display_name(target)));
            }

            // An untyped math result is computed again in the float it becomes, rounding once
            const auto known{comptime_value(op)};
            const auto origin{known && from.get_kind() == gs::type_kind::COMPTIME_FLOAT
                                  ? comp_.math_origin_of(*known)
                                  : stdx::none};
            if (const auto format{gs::float_format_of(target)}; origin && format) {
                const auto again{evaluate_traced(origin->function, origin->input, *format)};
                return operand{gir::value::constant(pool, again.value, target).constant_index()};
            }

            // An untyped literal rounds straight from its digits
            const auto format{gs::float_format_of(target)};
            if (const auto digits{known && format &&
                                          from.get_kind() == gs::type_kind::COMPTIME_FLOAT
                                      ? comp_.literal_digits_in(*known, *format)
                                      : stdx::none}) {
                return operand{gir::value::constant(pool, *digits, target).constant_index()};
            }

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

    // A captured slot's number is read before it widens
    if (op.is<slot_value>() && gs::is_numeric(from.get_kind()) &&
        gs::is_numeric(target.get_kind()) && gs::is_implicit_widenable(from, target)) {
        return coerce(operand{TRY(as_runtime(op, src))}, target, src);
    }

    if (const auto value{op.as_opt<gir::value>()}) {
        // An untyped number read at runtime from its slot is used as the target's
        if (gs::is_comptime_numeric(from.get_kind()) && gs::is_numeric(target.get_kind()) &&
            !gs::is_comptime_numeric(target.get_kind())) {
            return operand{gir::value{value->data, target}};
        }

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

auto analyzer::known_bytes(const ip::aggregate_value& agg, gs::type& elem)
    -> stdx::option<std::string> {
    if (!gs::is_same_unqualified(elem, comp_.ctx().get_int(8, false))) { return stdx::none; }
    std::string text;
    for (const auto element : agg.elements) {
        if (!element) { return stdx::none; }
        const auto byte{comp_.constants().value_at(*element).data.as_opt<u64>()};
        if (!byte || *byte > 0xFF) { return stdx::none; }
        text += static_cast<char>(*byte);
    }

    return text;
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

    // A compile-time slice holds its elements
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

auto analyzer::slice_literal_as(slice_literal lit, gs::type& target, const source& src)
    -> result<operand> {
    if (mod().tag_of(lit.init) != tag::init_array) {
        return comp_.unsupported(u_, src, "a `^.{ ... }` with named fields");
    }

    auto rec{record(lit.init)};
    rec.skip_ref();
    const auto  count{rec.refs_field().size()};
    const auto& slice{target.get_data().as<gs::types::slice>()};
    auto&       elem{slice.underlying};
    auto&       array{comp_.ctx().get_array(target.is_constant() ? gs::types::mut::CONSTANT
                                                                 : gs::types::mut::MUTABLE,
                                            slice.null_terminated,
                                            count,
                                            elem)};
    // At compile time the slice holds the elements themselves
    if (is_comptime()) {
        const auto value{TRY(init_as(lit.init, array, src))};
        const auto known{TRY(require_comptime(value, src))};
        if (const auto slice_value{comptime_slice_of(known, array, target)}) {
            return *slice_value;
        }

        return comp_.unsupported(u_, src, "a `^.{ ... }` at compile time");
    }

    // A known array lives in a hidden read-only global, so the slice may outlive the expression
    const auto before{comp_.speculate()};
    const auto folded{[&] {
        const auto scope{comptime_scope()};
        return init_as(lit.init, array, src);
    }()};
    const bool unsupported{!folded && folded.error() == stop::UNSUPPORTED};
    comp_.abandon(before, unsupported);
    if (unsupported) { return stdx::err{stop::UNSUPPORTED}; }

    gir::value address;
    if (const auto known{folded ? comptime_value(*folded) : stdx::option<ip::index>{}}) {
        auto  name{comp_.literal_global_name("slice_lit")};
        auto& global{comp_.gir().add_global(name, array, true, stdx::none, gir::linkage::INTERNAL)};
        global.const_init.emplace(*known);
        address = gir::value{builder_.emit_global_addr(std::move(name), array, true), array};
    } else {
        const auto value{TRY(init_as(lit.init, array, src))};
        address = spill(TRY(as_runtime(value, src)), array, false).address;
    }

    const auto decayed{TRY(slice_of_array(address, array))};
    return operand{gir::value{decayed.data, target}};
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

auto analyzer::comptime_referent(const operand& op) -> stdx::option<ip::index> {
    if (const auto memory{memory_place(op)}) { return comptime_value(load_comptime(*memory)); }
    const auto known{comptime_value(op)};
    if (!known || comp_.constants().is_type(*known)) { return stdx::none; }
    const auto address{comp_.constants().value_at(*known).data.as_opt<ip::address_value>()};
    if (!address || address->pointee.size() != 1) { return stdx::none; }
    return address->pointee.front();
}

auto analyzer::read_through_reference(const operand& op, const source& src) -> result<operand> {
    auto& type{type_of(op)};
    if (type.get_kind() != gs::type_kind::REFERENCE) { return op; }

    // A compile-time reference holds what it refers to, which runtime code reads where it lives
    const bool stored_address{!is_comptime() && !memory_place(op) && comptime_value(op)};
    if (const auto referent{comptime_referent(op)}; referent && !stored_address) {
        return operand{*referent};
    }
    if (is_comptime()) { return op; }
    auto&      referent{*gs::pointee_of(type)};
    const auto address{TRY(as_runtime(op, src))};
    return operand{
        gir::value{builder_.emit_load(gir::value{address.data, referent}, referent), referent}};
}

auto analyzer::reject_runtime_to_comptime(const operand& op, gs::type& target, const source& src)
    -> result<void> {
    // A compile-time number is only ever known, never read from a runtime value
    const auto value{op.as_opt<gir::value>()};
    if (!value || !value->type || !gs::is_comptime_numeric(target.get_kind()) || is_comptime() ||
        gs::is_comptime_numeric(value->type->get_kind()) ||
        !gs::is_numeric(value->type->get_kind())) {
        return {};
    }

    return comp_.report(u_,
                        src,
                        gs::error::COMPTIME_EVALUATION_FAILED,
                        fmt::format("a runtime '{}' cannot become '{}'; the value must be known at "
                                    "compile time",
                                    comp_.ctx().type_display_name(*value->type),
                                    comp_.ctx().type_display_name(target)));
}

auto analyzer::coerce_to(const operand&  op,
                         gs::type&       target,
                         const source&   src,
                         const use_site& site) -> result<operand> {
    TRY(reject_runtime_to_comptime(op, target, src));

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

    // An expression with an untyped side computes in the wider integer type it is used as
    if (const auto value{op.as_opt<gir::value>()};
        value && site.from && site.computes_in_target && value->type &&
        gs::is_integer(value->type->get_kind()) && gs::is_integer(target.get_kind())) {
        if (const auto widened{TRY(widened_arithmetic(*site.from, target, src))}) {
            return operand{*widened};
        }
    }

    if (auto coerced{TRY(coerce(op, target, src))}) { return std::move(*coerced); }
    auto&       ctx{comp_.ctx()};
    const auto& from{type_of(op)};

    // A type given where a value of a value type belongs
    if (const auto known{comptime_value(op)};
        known && comp_.constants().is_type(*known) && gs::is_value_type(target.get_kind())) {
        return comp_.report(u_,
                            src,
                            gs::error::TYPE_USED_AS_VALUE,
                            fmt::format("'{}' is a type, not a value",
                                        ctx.type_display_name(comp_.constants().type_at(*known))));
    }

    // A reference to a closure is the environment an erased `fn` holds
    if (const auto referent{gs::pointee_of(from)};
        from.get_kind() == gs::type_kind::REFERENCE && !is_comptime() && referent &&
        (comp_.closure_body(*referent) || comp_.closure_generic(*referent))) {
        if (const auto erased{TRY(as_erased_callable(op, target, src))}) { return *erased; }
    }

    // A reference is read where a value is wanted
    if (from.get_kind() == gs::type_kind::REFERENCE && !is_comptime() &&
        target.get_kind() != gs::type_kind::REFERENCE &&
        target.get_kind() != gs::type_kind::POINTER) {
        const auto read{TRY(read_through_reference(op, src))};
        return coerce_to(read, target, src, site);
    }

    // Functions as values arrive with erased callables and closures
    const auto callable{[](const gs::type& t) {
        const auto pointee{gs::pointee_of(t)};
        return t.get_kind() == gs::type_kind::FUNCTION || t.get_kind() == gs::type_kind::CLOSURE ||
               t.get_kind() == gs::type_kind::AUTO ||
               (pointee && pointee->get_kind() == gs::type_kind::FUNCTION);
    }};

    // A generic function's value holds nothing until it captures
    if (const auto g{op.as_opt<generic_ref>()};
        g && comp_.closure_generic(target).transform([&](const generic_fn& f) {
            return &f == g->fn.get();
        }) == stdx::option<bool>{true}) {
        const auto value{comp_.constants().intern(ip::undefined_value{}, target)};
        if (is_comptime()) { return operand{value}; }
        return operand{constant(value, target)};
    }

    // Only a callable converts to a callable, so anything else is a plain mismatch
    const bool from_callable{callable(from) || op.is<function_ref>() || op.is<generic_ref>()};
    if (from_callable || callable(target)) {
        if (const auto erased{TRY(as_erased_callable(op, target, src))}) { return *erased; }
    }
    if (from.get_kind() == gs::type_kind::VECTOR || target.get_kind() == gs::type_kind::VECTOR) {
        if (const auto converted{TRY(coerce_vector(op, from, target, src))}) { return *converted; }
    }
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

auto analyzer::peer_type(gsl::span<const operand>       ops,
                         const source&                  src,
                         stdx::option<std::string_view> what) -> result<gsl::not_null<gs::type*>> {
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
        auto message{gs::peer_error_message(ctx, peers, peer.error())};
        if (what) { message = fmt::format("'{}': {}", *what, message); }
        return comp_.report(u_, src, gs::error::NO_PEER_TYPE, std::move(message));
    }

    return *peer;
}

auto analyzer::literal_name(const enum_literal& literal) const -> std::string {
    if (literal.named) {
        return comp_.constants().value_at(*literal.named).data.as<ip::enum_value>().name;
    }

    return std::string{mod().string(mod().data_of(literal.inst).str)};
}

auto analyzer::literal_value(const enum_literal& literal) -> ip::index {
    if (literal.named) { return *literal.named; }
    return comp_.constants().intern(ip::enum_value{.name = literal_name(literal), .value = 0},
                                    builtin(gs::type_kind::ENUM_LITERAL));
}

auto analyzer::concrete(const operand& op, const source& src) -> result<operand> {
    if (const auto cast{op.as_opt<cast_ref>()}) { return cast_target_inferred(*cast, stdx::none); }
    if (const auto init{op.as_opt<anon_init>()}; init && mod().tag_of(init->inst) == tag::range) {
        return range_value(init->inst, stdx::none, src);
    }

    if (const auto m{op.as_opt<merge_ref>()}) {
        // A lone untyped arrival, like a block breaking out a range, is that value
        const auto& arrivals{merges_[m->index].arrivals};
        if (arrivals.size() == 1 && is_untyped_literal(arrivals.front().value)) {
            return concrete(arrivals.front().value, src);
        }

        if (is_untyped_literal(arrivals.front().value)) {
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

auto analyzer::stored_pointee(const ip::address_value& address, gs::type& type, const source& src)
    -> result<gir::value> {
    const auto pointee_type{gs::pointee_of(type)};
    if (address.pointee.size() != 1 || !pointee_type) {
        return comp_.unsupported(u_, src, "the address of a value with no runtime storage");
    }

    // A typed value is hoisted into a hidden read-only global
    const auto value{address.pointee.front()};
    if (!gs::is_comptime_numeric(pointee_type->get_kind())) {
        auto  name{comp_.literal_global_name("ref_lit")};
        auto& global{
            comp_.gir().add_global(name, *pointee_type, true, stdx::none, gir::linkage::INTERNAL)};
        global.const_init.emplace(value);
        return gir::value{builder_.emit_global_addr(std::move(name), *pointee_type, true), type};
    }

    // An untyped one has no global type, so it is kept in a slot
    const auto slot{spill(constant(value, *pointee_type), *pointee_type, false)};
    return gir::value{builder_.emit_address_of(slot.address, type), type};
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

        // A vector constant reads as its value, which the old emitter builds in place
        if (gs::is_vector(*global->type)) { return materialize(global->value, src); }
        return load(global_place(global->symbol, *global->type, false));
    }

    if (const auto index{comptime_value(typed)}) {
        if (comp_.constants().is_type(*index)) {
            return comp_.report(
                u_,
                src,
                gs::error::TYPE_USED_AS_VALUE,
                fmt::format("'{}' is a type, not a value",
                            comp_.ctx().type_display_name(comp_.constants().type_at(*index))));
        }

        const auto& data{comp_.constants().value_at(*index).data};
        if (data.is<ip::memory_value>()) {
            return comp_.unsupported(u_, src, "a compile-time address used at runtime");
        }

        if (data.is<ip::aggregate_value>() || data.is<ip::union_value>()) {
            TRY(require_runtime(src));
            return materialize(*index, src);
        }

        // A global's known address is read where it lives
        if (const auto address{data.as_opt<ip::address_value>()};
            address && !address->symbol.empty()) {
            TRY(require_runtime(src));
            auto&             type{type_of(typed)};
            const std::string symbol{address->symbol};
            return gir::value{builder_.emit_global_addr(symbol, type, type.is_constant()), type};
        }

        if (const auto address{data.as_opt<ip::address_value>()}) {
            TRY(require_runtime(src));
            return stored_pointee(*address, type_of(typed), src);
        }

        if (const auto text{data.as_opt<std::string>()};
            text && type_of(typed).get_kind() == gs::type_kind::SLICE) {
            TRY(require_runtime(src));
            return string_as_slice(*text, type_of(typed));
        }

        return constant(*index, type_of(typed));
    }

    if (typed.is<anon_init>() || typed.is<enum_literal>() || typed.is<implicit_call>()) {
        return comp_.unsupported(u_, src, "an untyped literal with no type to take");
    }

    // A function item's value is its code address, named by its symbol
    if (const auto fn{typed.as_opt<function_ref>()}; fn && !fn->fn->env) {
        TRY(require_runtime(src));
        comp_.request_body(*fn->fn);
        auto& type{*fn->fn->type};
        return constant(comp_.constants().intern(fn->fn->symbol, type), type);
    }

    if (typed.is<pack_ref>()) {
        return comp_.report(u_,
                            src,
                            gs::error::PACK_USE_OUT_OF_POSITION,
                            "A parameter pack is not a value; use 'rest.len', 'rest[k]', "
                            "'rest...', or an `unroll for` iterable");
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

        // Each arm converts where it arrives
        const auto arrivals{pending.arrivals};
        for (const auto& arrival : arrivals) {
            builder_.set_segment(*gir_fn_->get_segment(arrival.seg));
            builder_.set_splice(true);
            const auto stored{
                TRY(coerce_to(arrival.value, type, arrival.src, {.what = use_site::kind::STORE}))};
            const auto value{TRY(as_runtime(stored, arrival.src))};
            builder_.emit_store(slot, value);
        }
    }

    gir::value loaded{builder_.emit_load(slot, type), type};

    // Operators applied to the untyped merge run on it now that it has a type
    const auto ops{merges_[m.index].ops};
    for (const auto& op : ops) {
        const auto other{TRY(coerce_to(op.other, type, src, {.what = use_site::kind::STORE}))};
        const auto ov{TRY(as_runtime(other, src))};
        const auto int_op{
            gir::semantics::int_op_of(op.info.kind, op.info.wrapping, op.info.saturating)};
        const bool checked{int_op && checks_overflow(*int_op, type)};
        loaded = gir::value{builder_.emit_binary(op.info.kind,
                                                 op.merge_left ? loaded : ov,
                                                 op.merge_left ? ov : loaded,
                                                 type,
                                                 checked,
                                                 op.info.saturating),
                            type};
    }

    auto& done{merges_[m.index]};
    if (builder_.get_segment()->get_id() == done.seg) { done.materialized.emplace(loaded); }
    return loaded;
}

auto analyzer::union_tag_as_enum(const operand& op,
                                 gs::type&      union_type,
                                 gs::type&      enum_type,
                                 const source&  src) -> result<operand> {
    const auto& ut{union_type.get_data().as<gs::types::union_t>()};
    auto&       pool{comp_.constants()};
    const auto  slot{op.as_opt<slot_value>()};
    if (const auto known{slot && slot->known ? slot->known : comptime_value(op)}) {
        const auto un{pool.value_at(*known).data.as_opt<ip::union_value>()};
        if (!un) { return comp_.unsupported(u_, src, "a union's tag of this value"); }
        return enum_constant(enum_type, std::string{un->active_field}, src);
    }

    TRY(require_runtime(src));
    const auto base{TRY(operand_address(op, src))};
    auto&      i32_type{comp_.ctx().get_int(32, true)};
    auto&      bool_type{builtin(gs::type_kind::BOOL)};
    const auto tag_at{
        builder_.emit_get_element_ptr(base.address, {usize_constant(UNION_TAG_FIELD)}, i32_type)};
    const gir::value tag{builder_.emit_load(gir::value{tag_at, i32_type}, i32_type), i32_type};

    // The fields need not list the variants in order, so each tag index maps to its variant
    const gir::value result{builder_.emit_alloca(enum_type), enum_type};
    const auto       merge{new_segment()};
    for (usize i{0}; i < ut.field_names.size(); ++i) {
        const auto hit{new_segment()};
        const auto miss{new_segment()};
        const auto is_field{
            builder_.emit_binary(gir::instruction_kind::EQ,
                                 tag,
                                 constant(pool.intern(static_cast<i64>(i), i32_type), i32_type),
                                 bool_type)};
        builder_.emit_cond_goto(gir::value{is_field, bool_type}, hit, miss);
        switch_to(hit);
        const auto variant{TRY(enum_constant(enum_type, ut.field_names[i], src))};
        builder_.emit_store(result, TRY(as_runtime(variant, src)));
        emit_goto(merge);
        switch_to(miss);
    }

    builder_.emit_unreachable();
    switch_to(merge);
    return operand{gir::value{builder_.emit_load(result, enum_type), enum_type}};
}

} // namespace ghoti::uir::sema
