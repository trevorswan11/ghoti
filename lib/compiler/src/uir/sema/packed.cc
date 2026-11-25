#include <algorithm>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/int128.hh"

namespace ghoti::uir::sema {

namespace {

using gir::instruction_kind;

// A vector's lanes have no settled bit order inside a packed aggregate
[[nodiscard]] auto packed_vector_hint(const gs::type& field) -> std::string_view {
    if (!gs::is_vector(field)) { return ""; }
    return " (store a vector's lanes as `[N]T` instead)";
}

// `bool` is `i1` and a packed aggregate its own `iN`, so their bits are reinterpreted
[[nodiscard]] auto reinterprets_bits(gs::type_kind kind) -> bool {
    return gs::is_float(kind) || kind == gs::type_kind::BOOL || kind == gs::type_kind::STRUCT ||
           kind == gs::type_kind::UNION;
}

[[nodiscard]] auto is_address(gs::type_kind kind) -> bool {
    return kind == gs::type_kind::POINTER || kind == gs::type_kind::REFERENCE;
}

} // namespace

auto analyzer::check_packed_field(gs::type&        field,
                                  std::string_view name,
                                  std::string_view kind,
                                  const source&    src) -> result<void> {
    if (gs::packed_field_bits(field, comp_.ptr_bits())) { return {}; }
    return comp_.report(
        u_,
        src,
        gs::error::ILLEGAL_PACKED_FIELD,
        fmt::format("field '{}' of type '{}' cannot appear in a 'packed {}'; only integers, "
                    "floats, 'bool', enums, pointers, packed aggregates, and fixed arrays of "
                    "those are allowed{}",
                    name,
                    comp_.ctx().type_display_name(field),
                    kind,
                    packed_vector_hint(field)));
}

auto analyzer::reject_packed_alignment(std::string_view name,
                                       std::string_view kind,
                                       const source&    src) -> result<void> {
    return comp_.report(
        u_,
        src,
        gs::error::ILLEGAL_PACKED_FIELD,
        fmt::format("a 'packed {}' field cannot specify 'align' (field '{}')", kind, name));
}

auto analyzer::check_packed_width(u64 bits, std::string_view kind, const source& src)
    -> result<void> {
    if (bits >= 1 && bits <= 128) { return {}; }
    return comp_.report(u_,
                        src,
                        gs::error::ILLEGAL_PACKED_FIELD,
                        fmt::format("a 'packed {}' backing width of {} bits is unsupported (the "
                                    "current limit is 128 bits)",
                                    kind,
                                    bits));
}

auto analyzer::backing_int(u32 bits) -> gs::type& {
    return comp_.ctx().get_int(static_cast<u16>(bits), false);
}

void analyzer::zero_backing(const place& whole, u32 bits) {
    auto& backing_type{backing_int(bits)};
    builder_
        .emit_store(gir::value{whole.address.data, backing_type},
                    constant(comp_.constants().intern(u64{0}, backing_type), backing_type))
        .is_initializer = true;
}

auto analyzer::materialize_packed(const ip::value& value, gs::type& type, const source& src)
    -> result<gir::value> {
    const auto  ptr_bits{comp_.ptr_bits()};
    const auto  st{type.get_data().as_opt<gs::types::struct_t>()};
    const auto  slot{builder_.emit_alloca(type)};
    const place whole{.address = gir::value{slot, type}, .pointee = &type, .is_mutable = true};
    const auto  n{
        st ? gs::packed_backing_bits(*st, ptr_bits).value_or(1)
           : gs::packed_union_backing_bits(type.get_data().as<gs::types::union_t>(), ptr_bits)
                 .value_or(1)};
    zero_backing(whole, n);

    // Each field clears its bits in the backing, then sets the ones its known value has
    auto&      backing_type{backing_int(n)};
    const auto merge_known{[&](usize index, ip::index field) -> result<void> {
        auto&            field_type{field_type_at(type, index)};
        const auto       at{packed_layout(type, index, field_type)};
        const u128       field_mask{at.fbits >= 128 ? ~u128{0} : ((u128{1} << at.fbits) - 1)};
        const u128       all_ones{at.n >= 128 ? ~u128{0} : ((u128{1} << at.n) - 1)};
        const u128       mask{(field_mask << at.offset) & all_ones};
        const u128       bits{((TRY(bits_of(field, field_type, src)) & field_mask) << at.offset)};
        const gir::value old{builder_.emit_load(whole.address, backing_type), backing_type};
        const gir::value cleared{
            builder_.emit_binary(
                instruction_kind::AND,
                old,
                constant(comp_.constants().intern(all_ones ^ mask, backing_type), backing_type),
                backing_type),
            backing_type};
        const gir::value merged{
            builder_.emit_binary(
                instruction_kind::OR,
                cleared,
                constant(comp_.constants().intern(bits, backing_type), backing_type),
                backing_type),
            backing_type};
        builder_.emit_store(whole.address, merged);
        return {};
    }};

    if (const auto agg{value.data.as_opt<ip::aggregate_value>()}) {
        for (usize i{0}; i < agg->elements.size(); ++i) {
            if (agg->elements[i]) { TRY(merge_known(i, *agg->elements[i])); }
        }
    } else if (const auto un{value.data.as_opt<ip::union_value>()}; un && !un->payload.empty()) {
        const auto& ut{type.get_data().as<gs::types::union_t>()};
        const auto  field{std::ranges::find(ut.field_names, un->active_field)};
        TRY(merge_known(static_cast<usize>(field - ut.field_names.begin()), un->payload.front()));
    }

    return load(place{.address = whole.address, .pointee = &type, .is_mutable = false});
}

auto analyzer::field_type_at(gs::type& type, usize index) -> gs::type& {
    if (const auto st{type.get_data().as_opt<gs::types::struct_t>()}) { return st->type_at(index); }
    return type.get_data().as<gs::types::union_t>().type_at(index);
}

auto analyzer::packed_layout(gs::type& type, usize index, gs::type& field) -> packed_bits {
    const auto  ptr_bits{comp_.ptr_bits()};
    packed_bits at{.n = 1, .offset = 0, .fbits = 1};
    if (const auto st{type.get_data().as_opt<gs::types::struct_t>()}) {
        at.n      = gs::packed_backing_bits(*st, ptr_bits).value_or(1);
        at.offset = gs::packed_field_offset(*st, index, ptr_bits);
    } else {
        at.n = gs::packed_union_backing_bits(type.get_data().as<gs::types::union_t>(), ptr_bits)
                   .value_or(1);
    }

    at.fbits = gs::packed_field_bits(field, ptr_bits).value_or(at.n);
    return at;
}

auto analyzer::packed_place(const place& base, usize index, gs::type& field) -> place {
    auto at{packed_layout(*base.pointee, index, field)};
    if (base.packed) {
        at.parent = static_cast<u32>(packed_parents_.size());
        packed_parents_.emplace_back(base);
    }

    return place{
        .address = base.address, .pointee = &field, .is_mutable = base.is_mutable, .packed = at};
}

auto analyzer::packed_extract(const gir::value& object,
                              gs::type&         type,
                              usize             index,
                              gs::type&         field) -> gir::value {
    const auto at{packed_layout(type, index, field)};
    return bits_extract(gir::value{object.data, backing_int(at.n)}, at, field);
}

auto analyzer::packed_read(const place& p) -> gir::value {
    const auto& at{*p.packed};
    auto&       backing_type{backing_int(at.n)};
    if (at.parent) {
        const auto parent{packed_parents_[*at.parent]};
        return bits_extract(gir::value{packed_read(parent).data, backing_type}, at, *p.pointee);
    }

    const gir::value backing{builder_.emit_load(p.address, backing_type), backing_type};
    return bits_extract(backing, at, *p.pointee);
}

void analyzer::packed_store(const place& p, const gir::value& value) {
    const auto& at{*p.packed};
    auto&       backing_type{backing_int(at.n)};

    // A field of a packed field merges into that field's bits, which are then written in turn
    if (at.parent) {
        const auto       parent{packed_parents_[*at.parent]};
        const gir::value old{packed_read(parent).data, backing_type};
        packed_store(parent, bits_merge(old, at, *p.pointee, value));
        return;
    }

    const gir::value old{builder_.emit_load(p.address, backing_type), backing_type};
    builder_.emit_store(p.address, bits_merge(old, at, *p.pointee, value));
}

auto analyzer::bits_extract(const gir::value& backing, const packed_bits& at, gs::type& field)
    -> gir::value {
    auto&            backing_type{backing_int(at.n)};
    const gir::value shifted{
        at.offset == 0 ? backing
                       : gir::value{builder_.emit_binary(instruction_kind::SHR,
                                                         backing,
                                                         constant(comp_.constants().intern(
                                                                      u64{at.offset}, backing_type),
                                                                  backing_type),
                                                         backing_type),
                                    backing_type}};

    const auto kind{field.get_kind()};
    if (kind == gs::type_kind::INT) {
        const auto cast{gs::int_width(field) < at.n ? instruction_kind::TRUNC_CAST
                                                    : instruction_kind::WIDEN_CAST};
        return gir::value{builder_.emit_cast(cast, shifted, field), field};
    }

    auto&      narrow{backing_int(at.fbits)};
    const auto cast{at.fbits < at.n ? instruction_kind::TRUNC_CAST : instruction_kind::WIDEN_CAST};
    const gir::value narrowed{builder_.emit_cast(cast, shifted, narrow), narrow};
    if (reinterprets_bits(kind)) {
        return gir::value{builder_.emit_cast(instruction_kind::BIT_CAST, narrowed, field), field};
    }

    if (is_address(kind)) {
        return gir::value{builder_.emit_cast(instruction_kind::PTR_FROM_INT, narrowed, field),
                          field};
    }

    // An enum or `usize` is the field-width integer reinterpreted
    if (gs::is_same_unqualified(narrow, field)) { return narrowed; }
    return gir::value{builder_.emit_cast(instruction_kind::BIT_CAST, narrowed, field), field};
}

auto analyzer::bits_merge(const gir::value&  old,
                          const packed_bits& at,
                          gs::type&          field,
                          const gir::value&  value) -> gir::value {
    auto& backing_type{backing_int(at.n)};
    auto& narrow{backing_int(at.fbits)};

    const auto kind{field.get_kind()};
    gir::value as_bits{value};
    if (reinterprets_bits(kind)) {
        as_bits = gir::value{builder_.emit_cast(instruction_kind::BIT_CAST, value, narrow), narrow};
    } else if (is_address(kind)) {
        as_bits =
            gir::value{builder_.emit_cast(instruction_kind::INT_FROM_PTR, value, narrow), narrow};
    } else {
        const auto value_bits{
            value.type ? gs::packed_field_bits(*value.type, comp_.ptr_bits()).value_or(at.fbits)
                       : at.fbits};
        if (at.fbits < value_bits) {
            as_bits =
                gir::value{builder_.emit_cast(instruction_kind::TRUNC_CAST, value, narrow), narrow};
        } else if (at.fbits > value_bits) {
            as_bits =
                gir::value{builder_.emit_cast(instruction_kind::WIDEN_CAST, value, narrow), narrow};
        } else if (value.type && !gs::is_same_unqualified(*value.type, narrow)) {
            as_bits =
                gir::value{builder_.emit_cast(instruction_kind::BIT_CAST, value, narrow), narrow};
        }
    }

    const auto int_constant{
        [&](u128 v) { return constant(comp_.constants().intern(v, backing_type), backing_type); }};
    const gir::value widened{
        builder_.emit_cast(instruction_kind::WIDEN_CAST, as_bits, backing_type), backing_type};
    const gir::value shifted{
        at.offset == 0 ? widened
                       : gir::value{builder_.emit_binary(instruction_kind::SHL,
                                                         widened,
                                                         constant(comp_.constants().intern(
                                                                      u64{at.offset}, backing_type),
                                                                  backing_type),
                                                         backing_type),
                                    backing_type}};

    const u128 field_mask{at.fbits >= 128 ? ~u128{0} : ((u128{1} << at.fbits) - 1)};
    const u128 all_ones{at.n >= 128 ? ~u128{0} : ((u128{1} << at.n) - 1)};
    const u128 mask{(field_mask << at.offset) & all_ones};

    const gir::value bits_in{
        builder_.emit_binary(instruction_kind::AND, shifted, int_constant(mask), backing_type),
        backing_type};
    const gir::value cleared{
        builder_.emit_binary(
            instruction_kind::AND, old, int_constant(all_ones ^ mask), backing_type),
        backing_type};
    return gir::value{builder_.emit_binary(instruction_kind::OR, cleared, bits_in, backing_type),
                      backing_type};
}

} // namespace ghoti::uir::sema
