#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <gsl/pointers>
#include <gsl/span>
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

// The index of the field `name` among `names`, none when there is no such field
[[nodiscard]] auto index_of(gsl::span<const std::string_view> names, std::string_view name)
    -> stdx::option<usize> {
    const auto found{std::ranges::find(names, name)};
    if (found == names.end()) { return stdx::none; }
    return static_cast<usize>(found - names.begin());
}

} // namespace

auto analyzer::spill(const gir::value& v, gs::type& type, bool to_entry) -> place {
    if (to_entry) {
        const auto slot{builder_.emit_alloca_in_entry(type)};
        builder_.emit_store_in_entry(slot, v);
        return place{.address = gir::value{slot, type}, .pointee = &type, .is_mutable = false};
    }

    const auto slot{builder_.emit_alloca(type)};
    builder_.emit_store(slot, v).is_initializer = true;
    return place{.address = gir::value{slot, type}, .pointee = &type, .is_mutable = false};
}

auto analyzer::load(const place& p) -> gir::value {
    return gir::value{builder_.emit_load(p.address, *p.pointee), *p.pointee};
}

auto analyzer::settle(const place& p) -> place {
    if (!p.pending) { return p; }
    const auto at{builder_.emit_get_element_ptr(
        p.pending->union_address, {usize_constant(UNION_PAYLOAD_FIELD)}, *p.pointee)};
    return place{
        .address    = gir::value{at, *p.pointee},
        .pointee    = p.pointee,
        .is_mutable = p.is_mutable,
    };
}

auto analyzer::read(const place& p, const source& src) -> result<gir::value> {
    if (p.pending) {
        TRY(check_active_field(p.pending->union_address, p.pending->index, p.pending->name, src));
    }

    return load(settle(p));
}

auto analyzer::global_place(std::string_view symbol, gs::type& type, bool is_mutable) -> place {
    const auto address{builder_.emit_global_addr(std::string{symbol}, type, !is_mutable)};
    return place{.address = gir::value{address, type}, .pointee = &type, .is_mutable = is_mutable};
}

auto analyzer::address_of(ref r, const source& src) -> result<place> {
    TRY(require_runtime(src));

    // A read of a `let mut` is addressed where the variable lives, not through a copy
    if (const auto inst{as_inst(r)}; inst && mod().tag_of(*inst) == tag::load) {
        const auto target{TRY(resolve(mod().data_of(*inst).un.operand, src))};
        if (const auto p{target.as_opt<place>()}; p && p->address.type) { return *p; }
    }

    const auto op{TRY(resolve(r, src))};
    if (const auto p{op.as_opt<place>()}) {
        if (!p->pending && !p->address.type) {
            return comp_.unsupported(u_, src, "an untyped `let mut`'s address");
        }

        return settle(*p);
    }

    if (const auto s{op.as_opt<slot_value>()}) {
        return place{.address = s->address, .pointee = s->type, .is_mutable = s->is_mutable};
    }

    if (const auto g{op.as_opt<global_ref>()}) { return global_place(*g->symbol, *g->type, true); }
    if (const auto g{op.as_opt<global_const>()}) {
        return global_place(*g->symbol, *g->type, false);
    }

    if (op.is<comptime_place>() || op.is<function_ref>() || op.is<anon_init>() ||
        op.is<enum_literal>()) {
        return comp_.unsupported(u_, src, "the address of a value with no runtime storage");
    }

    // A value with no storage is spilled into a slot
    const auto inst{as_inst(r)};
    const bool binding{inst && is_binding(mod().tag_of(*inst))};
    const bool to_entry{binding && (op.is<ip::index>() || mod().tag_of(*inst) == tag::param)};

    if (const auto kind{type_of(op).get_kind()};
        kind == gs::type_kind::COMPTIME_INT || kind == gs::type_kind::COMPTIME_FLOAT) {
        return comp_.unsupported(u_, src, "the address of an untyped constant");
    }

    const auto value{TRY(as_runtime(op, src))};
    auto&      type{*value.type};
    const auto slot{spill(value, type, to_entry)};
    if (binding) {
        values_.insert_or_assign(*inst, slot_value{.address = slot.address, .type = &type});
    }

    return slot;
}

auto analyzer::binding_view(ref r, gs::type& type) -> gs::type& {
    const auto kind{type.get_kind()};
    if (kind != gs::type_kind::SLICE && kind != gs::type_kind::STRUCT &&
        kind != gs::type_kind::UNION) {
        return type;
    }

    const auto inst{as_inst(r)};
    if (!inst) { return type; }
    auto& pool{comp_.ctx().pool};
    switch (mod().tag_of(*inst)) {
    case tag::param:
    case tag::load:      return *pool.with_const(type, false);
    case tag::let_value: return *pool.with_const(type, true);
    default:             return type;
    }
}

auto analyzer::through_pointer(const place& base, const source& src) -> result<place> {
    auto& type{*base.pointee};
    if (const auto r{type.get_data().as_opt<gs::types::reference>()}) {
        const auto loaded{load(base)};
        return place{
            .address = loaded, .pointee = &r->underlying, .is_mutable = !type.is_constant()};
    }

    if (const auto p{type.get_data().as_opt<gs::types::pointer>()}) {
        const auto loaded{load(base)};
        TRY(check_not_null(loaded, src));
        return place{
            .address = loaded, .pointee = &p->underlying, .is_mutable = !type.is_constant()};
    }

    return base;
}

auto analyzer::field_index(gs::type& type, std::string_view name, const source& src)
    -> result<usize> {
    TRY(comp_.require_fields(u_, src, type));
    const auto names{[&] -> gsl::span<const std::string_view> {
        if (const auto st{type.get_data().as_opt<gs::types::struct_t>()}) {
            return st->field_names;
        }

        if (const auto ut{type.get_data().as_opt<gs::types::union_t>()}) { return ut->field_names; }
        return {};
    }()};
    if (const auto found{index_of(names, name)}) { return *found; }
    return comp_.report(u_,
                        src,
                        gs::error::UNDECLARED_IDENTIFIER,
                        fmt::format("Type '{}' has no field named '{}'",
                                    comp_.ctx().type_display_name(type),
                                    name));
}

auto analyzer::mutable_view(gs::type& field, bool through_mutable) -> gs::type& {
    // An array field reached through a mutable place has mutable elements
    const auto array{field.get_data().as_opt<gs::types::array>()};
    if (!array || !through_mutable || !field.is_constant()) { return field; }
    return comp_.ctx().get_array(
        gs::types::mut::MUTABLE, array->null_terminated, array->len, array->underlying);
}

auto analyzer::field_place(const place& object, std::string_view name, const source& src)
    -> result<place> {
    const auto base{TRY(through_pointer(object, src))};
    auto&      type{*base.pointee};
    auto&      ctx{comp_.ctx()};
    if (const auto s{type.get_data().as_opt<gs::types::slice>()}) {
        const bool is_len{name == "len"};
        if (!is_len && name != "ptr") {
            return comp_.report(u_,
                                src,
                                gs::error::UNDECLARED_IDENTIFIER,
                                fmt::format("Type '{}' has no field named '{}'",
                                            ctx.type_display_name(type),
                                            name));
        }

        auto&      field_type{is_len ? builtin(gs::type_kind::USIZE)
                                     : ctx.get_pointer(type.get_key().get_mut(), s->underlying)};
        const auto at{builder_.emit_get_element_ptr(
            base.address,
            {usize_constant(is_len ? SLICE_LEN_FIELD : SLICE_PTR_FIELD)},
            field_type)};
        return place{
            .address    = gir::value{at, field_type},
            .pointee    = &field_type,
            .is_mutable = base.is_mutable,
        };
    }

    if (type.get_kind() != gs::type_kind::STRUCT && type.get_kind() != gs::type_kind::UNION) {
        return comp_.unsupported(u_, src, fmt::format("a field of a `{}`", type.to_string()));
    }

    const auto index{TRY(field_index(type, name, src))};
    if (const auto ut{type.get_data().as_opt<gs::types::union_t>()}) {
        auto& declared{ut->type_at(index)};
        auto& field_type{mutable_view(declared, base.is_mutable)};
        if (ut->is_untagged) {
            return place{.address    = gir::value{base.address.data, field_type},
                         .pointee    = &field_type,
                         .is_mutable = base.is_mutable};
        }

        // The payload's address waits, since a read checks the tag first
        return place{
            .address    = {},
            .pointee    = &field_type,
            .is_mutable = base.is_mutable,
            .pending    = union_field{.union_address = base.address,
                                      .index         = index,
                                      .name          = ut->field_names[index]},
        };
    }

    auto& field_type{
        mutable_view(type.get_data().as<gs::types::struct_t>().type_at(index), base.is_mutable)};
    const auto at{builder_.emit_get_element_ptr(base.address, {usize_constant(index)}, field_type)};
    return place{
        .address    = gir::value{at, field_type},
        .pointee    = &field_type,
        .is_mutable = base.is_mutable,
    };
}

auto analyzer::analyze_field_val(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  object_ref{*rec.ref_field()};
    const auto  name{rec.str_field()};
    const auto  object{TRY(resolve(object_ref, src))};

    if (const auto index{comptime_value(object)}) {
        if (comp_.constants().is_type(*index)) {
            return member_of(comp_.constants().type_at(*index), name, src);
        }

        return comptime_field(*index, name, src);
    }

    if (const auto g{object.as_opt<global_const>()}; g && is_comptime()) {
        return comptime_field(g->value, name, src);
    }

    if (object.is<function_ref>()) { return comp_.unsupported(u_, src, "a field of a function"); }
    TRY(require_runtime(src));

    const auto object_place{TRY(address_of(object_ref, src))};
    const auto base{TRY(through_pointer(object_place, src))};
    auto&      type{*base.pointee};
    if (const auto arr{type.get_data().as_opt<gs::types::array>()}) {
        if (name == "len") {
            return comp_.constants().intern(u64{arr->len}, builtin(gs::type_kind::USIZE));
        }

        if (name == "ptr") {
            auto&      ptr_type{comp_.ctx().get_pointer(type.get_key().get_mut(), arr->underlying)};
            const auto at{builder_.emit_get_element_ptr(
                base.address, {usize_constant(ARRAY_PTR_FIELD)}, ptr_type)};
            return gir::value{at, ptr_type};
        }
    }

    const auto field{TRY(field_place(base, name, src))};
    return operand{TRY(read(field, src))};
}

auto analyzer::analyze_field_ptr(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  object_ref{*rec.ref_field()};
    const auto  name{rec.str_field()};
    const bool  assigned{(rec.u32_field() & flags::ASSIGNED) != 0};
    const auto  object{TRY(resolve(object_ref, src))};

    // `T.member` names the member's own storage
    if (const auto index{comptime_value(object)}; index && comp_.constants().is_type(*index)) {
        const auto member{TRY(member_of(comp_.constants().type_at(*index), name, src))};
        if (is_comptime()) {
            if (const auto g{member.as_opt<global_const>()}) { return g->value; }
            return member;
        }

        if (const auto g{member.as_opt<global_ref>()}) {
            return global_place(*g->symbol, *g->type, true);
        }

        if (const auto g{member.as_opt<global_const>()}) {
            return global_place(*g->symbol, *g->type, false);
        }

        return member;
    }

    if (is_comptime() || object.is<comptime_place>()) {
        return comp_.unsupported(u_, src, "a field of comptime memory");
    }

    // A field of a compile-time value is known, so reading it needs no storage
    if (const auto known{comptime_value(object)}; known && !assigned) {
        return comptime_field(*known, name, src);
    }

    const auto base{TRY(address_of(object_ref, src))};
    const auto field{TRY(field_place(base, name, src))};

    // Assigning a tagged union's field makes it the active one before the value is computed
    if (assigned && field.pending) {
        auto&      i32_type{comp_.ctx().get_int(32, true)};
        const auto tag_ptr{builder_.emit_get_element_ptr(
            field.pending->union_address, {usize_constant(UNION_TAG_FIELD)}, i32_type)};
        builder_
            .emit_store(
                gir::value{tag_ptr, i32_type},
                constant(comp_.constants().intern(static_cast<i64>(field.pending->index), i32_type),
                         i32_type))
            .is_initializer = true;
        // The payload's address comes before the value too
        return settle(field);
    }

    return field;
}

auto analyzer::index_operand(ref index_ref, const source& src) -> result<gir::value> {
    const auto index{TRY(resolve_value(index_ref, src))};
    auto&      type{type_of(index)};
    if (type.is_poison()) { return stdx::err{stop::POISON}; }
    if (type.get_kind() == gs::type_kind::COMPTIME_INT) {
        return as_runtime(
            TRY(coerce_to(
                index, builtin(gs::type_kind::USIZE), src, {.what = use_site::kind::STORE})),
            src);
    }

    if (!gs::is_integer(type.get_kind())) {
        return comp_.report(u_,
                            src,
                            gs::error::TYPE_MISMATCH,
                            fmt::format("An index must be an integer; found '{}'",
                                        comp_.ctx().type_display_name(type)));
    }

    return as_runtime(index, src);
}

auto analyzer::bounds_check(const gir::value& index,
                            const gir::value& bound,
                            bool              sentinel,
                            const source&     src) -> result<void> {
    auto&      bool_type{builtin(gs::type_kind::BOOL)};
    const auto ok{builder_.emit_binary(
        sentinel ? gir::instruction_kind::LE : gir::instruction_kind::LT, index, bound, bool_type)};
    return guard(gir::value{ok, bool_type}, "index out of bounds", src);
}

auto analyzer::elem_place(const place& object, ref index_ref, const source& src) -> result<place> {
    const auto base{TRY(through_pointer(object, src))};
    const auto index{TRY(index_operand(index_ref, src))};
    auto&      type{*base.pointee};
    auto&      ctx{comp_.ctx()};
    const bool safety{ctx.runtime_safety};

    // A signed index is compared as an `isize`
    const bool is_signed{index.type && gs::is_signed_integer(*index.type)};
    auto&      index_type{builtin(is_signed ? gs::type_kind::ISIZE : gs::type_kind::USIZE)};
    const auto compared{[&] {
        if (index.type && index.type->get_kind() != index_type.get_kind()) {
            return gir::value{
                builder_.emit_cast(gir::instruction_kind::WIDEN_CAST, index, index_type),
                index_type};
        }

        return index;
    }};

    if (const auto arr{type.get_data().as_opt<gs::types::array>()}) {
        if (safety) {
            const auto bound{
                constant(comp_.constants().intern(u64{arr->len}, index_type), index_type)};
            TRY(bounds_check(compared(), bound, arr->null_terminated, src));
        }

        auto&      elem{*ctx.pool.with_const(arr->underlying, type.is_constant())};
        const auto at{builder_.emit_get_element_ptr(base.address, {index}, elem)};
        return place{
            .address    = gir::value{at, elem},
            .pointee    = &elem,
            .is_mutable = base.is_mutable && !type.is_constant(),
        };
    }

    if (const auto s{type.get_data().as_opt<gs::types::slice>()}) {
        auto& ptr_type{
            ctx.get_pointer(type.is_constant() ? gs::types::mut::CONSTANT : gs::types::mut::MUTABLE,
                            s->underlying)};
        const auto ptr_slot{builder_.emit_get_element_ptr(
            base.address, {usize_constant(SLICE_PTR_FIELD)}, ptr_type)};
        const auto ptr{builder_.emit_load(gir::value{ptr_slot, ptr_type}, ptr_type)};
        if (safety) {
            // The index converts before the length is read
            const auto checked{compared()};
            auto&      usize_type{builtin(gs::type_kind::USIZE)};
            const auto len_slot{builder_.emit_get_element_ptr(
                base.address, {usize_constant(SLICE_LEN_FIELD)}, usize_type)};
            gir::value bound{builder_.emit_load(gir::value{len_slot, usize_type}, usize_type),
                             usize_type};
            if (is_signed) {
                bound = gir::value{
                    builder_.emit_cast(gir::instruction_kind::BIT_CAST, bound, index_type),
                    index_type};
            }

            TRY(bounds_check(checked, bound, false, src));
        }

        auto&      elem{*ctx.pool.with_const(s->underlying, type.is_constant())};
        const auto at{builder_.emit_get_element_ptr(gir::value{ptr, ptr_type}, {index}, elem)};
        return place{
            .address = gir::value{at, elem}, .pointee = &elem, .is_mutable = !type.is_constant()};
    }

    return comp_.unsupported(u_, src, fmt::format("indexing a `{}`", type.to_string()));
}

auto analyzer::analyze_elem_val(inst_id inst) -> result<operand> {
    const auto& d{mod().data_of(inst)};
    const auto& src{src_of(inst)};
    const auto  object{TRY(resolve(d.bin.lhs, src))};
    if (const auto index{comptime_value(object)}) {
        if (const auto at{comptime_value(TRY(resolve_value(d.bin.rhs, src)))}) {
            return comptime_elem(*index, *at, src);
        }
    }

    if (const auto g{object.as_opt<global_const>()}; g && is_comptime()) {
        if (const auto at{comptime_value(TRY(resolve_value(d.bin.rhs, src)))}) {
            return comptime_elem(g->value, *at, src);
        }
    }

    TRY(require_runtime(src));
    const auto object_place{TRY(address_of(d.bin.lhs, src))};
    return operand{load(TRY(elem_place(object_place, d.bin.rhs, src)))};
}

auto analyzer::analyze_elem_ptr(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  object_ref{*rec.ref_field()};
    const auto  index_ref{*rec.ref_field()};
    if (is_comptime() || TRY(resolve(object_ref, src)).is<comptime_place>()) {
        return comp_.unsupported(u_, src, "an element of comptime memory");
    }

    const auto object_place{TRY(address_of(object_ref, src))};
    const auto elem{TRY(elem_place(object_place, index_ref, src))};
    return operand{elem};
}

auto analyzer::analyze_ref_of(inst_id inst, bool is_pointer) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  operand_ref{*rec.ref_field()};
    const auto  bits{rec.u32_field()};
    const bool  is_mut{(bits & flags::MUT) != 0};
    const auto  target{TRY(resolve(operand_ref, src))};
    auto&       ctx{comp_.ctx()};
    const auto  mut{is_mut ? gs::types::mut::MUTABLE : gs::types::mut::CONSTANT};

    // `^T` of a type is the pointer type
    if (const auto index{comptime_value(target)}; index && comp_.constants().is_type(*index)) {
        auto& pointee{comp_.constants().type_at(*index)};
        return interned_type(is_pointer ? ctx.get_pointer(mut, pointee)
                                        : ctx.get_reference(mut, pointee));
    }

    if (is_comptime()) { return comp_.unsupported(u_, src, "an address at compile time"); }

    auto& target_type{type_of(target)};
    if (target_type.is_poison()) { return stdx::err{stop::POISON}; }
    if (target.is<function_ref>() || target_type.get_kind() == gs::type_kind::REFERENCE) {
        return comp_.unsupported(u_, src, "the address of a function or reference");
    }

    const auto slot{TRY(address_of(operand_ref, src))};
    if (is_mut && !slot.is_mutable) {
        // `^mut a[i]` of an immutable element is an error, just like `a[i] = v`
        const auto target_inst{as_inst(operand_ref)};
        const bool element{target_inst && mod().tag_of(*target_inst) == tag::elem_ptr};
        return comp_.report(u_,
                            src,
                            gs::error::ASSIGNMENT_TO_CONST,
                            std::string{element ? messages::MUTABLE_ADDRESS_OF_ELEMENT
                                                : messages::MUTABLE_ADDRESS_OF_IMMUTABLE});
    }

    auto& pointee{*slot.pointee};
    auto& result_type{is_pointer ? ctx.get_pointer(mut, pointee) : ctx.get_reference(mut, pointee)};
    return gir::value{builder_.emit_address_of(slot.address, result_type), result_type};
}

auto analyzer::pointer_place(const operand& ptr, const source& src) -> result<place> {
    auto& type{type_of(ptr)};
    if (type.is_poison()) { return stdx::err{stop::POISON}; }
    const auto referent{gs::pointee_of(type)};
    if (!referent) {
        return comp_.report(u_,
                            src,
                            gs::error::TYPE_MISMATCH,
                            fmt::format("Cannot dereference a value of type '{}'",
                                        comp_.ctx().type_display_name(type)));
    }

    const auto value{TRY(as_runtime(ptr, src))};
    TRY(check_not_null(value, src));
    return place{.address = value, .pointee = &*referent, .is_mutable = !type.is_constant()};
}

auto analyzer::analyze_deref(inst_id inst) -> result<operand> {
    const auto& d{mod().data_of(inst)};
    const auto& src{src_of(inst)};
    const auto  ptr{TRY(resolve_value(d.un.operand, src))};
    if (is_comptime()) { return comp_.unsupported(u_, src, "a dereference at compile time"); }
    auto& type{type_of(ptr)};
    if (type.get_kind() == gs::type_kind::SLICE || gs::is_fat_callable(type)) {
        return comp_.unsupported(u_, src, "dereferencing a slice or callable");
    }

    const auto target{TRY(pointer_place(ptr, src))};
    if ((d.un.aux & flags::DEREF_PLACE) != 0) { return target; }
    // An aggregate is read where it lies, so a field or element of it is addressed in place
    if (gs::is_structural(target.pointee->get_kind())) {
        return slot_value{
            .address = target.address, .type = target.pointee, .is_mutable = target.is_mutable};
    }

    return operand{load(target)};
}

auto analyzer::operand_address(const operand& op, const source& src) -> result<place> {
    TRY(require_runtime(src));
    if (const auto s{op.as_opt<slot_value>()}) {
        return place{.address = s->address, .pointee = s->type, .is_mutable = s->is_mutable};
    }

    if (const auto p{op.as_opt<place>()}) { return settle(*p); }
    if (const auto g{op.as_opt<global_ref>()}) { return global_place(*g->symbol, *g->type, true); }
    if (const auto g{op.as_opt<global_const>()}) {
        return global_place(*g->symbol, *g->type, false);
    }

    // A named value keeps the slot it is spilled to
    if (const auto v{op.as_opt<gir::value>()}) {
        stdx::option<inst_id> binding;
        for (const auto& [inst, value] : values_) {
            const auto bound{value.as_opt<gir::value>()};
            const auto t{mod().tag_of(inst)};
            if (bound && *bound == *v && (is_binding(t) || t == tag::load)) {
                binding = inst;
                break;
            }
        }

        if (binding) { return address_of(inst_ref(*binding), src); }
    }

    const auto value{TRY(as_runtime(op, src))};
    return spill(value, *value.type, false);
}

auto analyzer::comptime_field(ip::index object, std::string_view name, const source& src)
    -> result<operand> {
    auto&       pool{comp_.constants()};
    const auto& value{pool.value_at(object)};
    auto&       type{value.type.value_or(builtin(gs::type_kind::VOID_))};
    if (const auto arr{type.get_data().as_opt<gs::types::array>()}; arr && name == "len") {
        return pool.intern(u64{arr->len}, builtin(gs::type_kind::USIZE));
    }

    if (const auto text{value.data.as_opt<std::string>()};
        text && type.get_kind() == gs::type_kind::SLICE && name == "len") {
        return pool.intern(u64{text->size()}, builtin(gs::type_kind::USIZE));
    }

    if (const auto agg{value.data.as_opt<ip::aggregate_value>()};
        agg && type.get_kind() == gs::type_kind::STRUCT) {
        const auto index{TRY(field_index(type, name, src))};
        const auto element{agg->elements[index]};
        auto&      field_type{type.get_data().as<gs::types::struct_t>().type_at(index)};
        if (!element) { return pool.intern(ip::undefined_value{}, field_type); }
        return *element;
    }

    if (const auto un{value.data.as_opt<ip::union_value>()}) {
        const auto index{TRY(field_index(type, name, src))};
        if (un->active_field != name) {
            return comp_.report(u_,
                                src,
                                gs::error::COMPTIME_EVALUATION_FAILED,
                                fmt::format("accessed inactive union field '{}'", name));
        }

        auto& field_type{type.get_data().as<gs::types::union_t>().type_at(index)};
        if (un->payload.empty()) { return pool.intern(ip::void_value{}, field_type); }
        return un->payload.front();
    }

    return comp_.unsupported(u_, src, fmt::format("a field of a comptime `{}`", type.to_string()));
}

auto analyzer::comptime_elem(ip::index object, ip::index at, const source& src) -> result<operand> {
    auto&       pool{comp_.constants()};
    const auto& value{pool.value_at(object)};
    const auto  agg{value.data.as_opt<ip::aggregate_value>()};
    const auto  index{int_of(pool.value_at(at))};
    if (!agg || !index || !value.type || !value.type->get_data().is<gs::types::array>()) {
        return comp_.unsupported(u_, src, "indexing this comptime value");
    }

    const auto small{index->to_i128()};
    if (!small || *small < 0 || *small >= static_cast<i128>(agg->elements.size())) {
        return comp_.report(u_, src, gs::error::COMPTIME_EVALUATION_FAILED, "index out of bounds");
    }

    const auto element{agg->elements[static_cast<usize>(*small)]};
    auto&      elem_type{value.type->get_data().as<gs::types::array>().underlying};
    return element.value_or(pool.intern(ip::undefined_value{}, elem_type));
}

} // namespace ghoti::uir::sema
