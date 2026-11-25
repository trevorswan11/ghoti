#include <algorithm>
#include <string>
#include <string_view>
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

[[nodiscard]] auto is_tagged_union(const gs::type& type) -> bool {
    const auto ut{type.get_data().as_opt<gs::types::union_t>()};
    return ut && !ut->is_untagged;
}

// Whether a value of the type holds a `type`, so it exists only at compile time
[[nodiscard]] auto holds_type(const gs::type& type, u32 depth = 0) -> bool {
    if (depth > 8) { return false; }
    if (type.get_kind() == gs::type_kind::TYPE) { return true; }
    if (const auto arr{type.get_data().as_opt<gs::types::array>()}) {
        return holds_type(arr->underlying, depth + 1);
    }

    if (const auto s{type.get_data().as_opt<gs::types::slice>()}) {
        return holds_type(s->underlying, depth + 1);
    }

    if (const auto st{type.get_data().as_opt<gs::types::struct_t>()}) {
        return std::ranges::any_of(st->fields, [&](stdx::option<gs::type&> field) {
            return field && holds_type(*field, depth + 1);
        });
    }

    return false;
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
    if (p.packed) { return packed_read(p); }
    if (p.lane) {
        return gir::value{builder_.emit_vector_op(gir::instruction_kind::EXTRACT_LANE,
                                                  {p.lane->vector, p.lane->index},
                                                  *p.pointee),
                          *p.pointee};
    }

    // A volatile load still reads through the volatile type, but the value it gives is plain
    auto& pointee{*p.pointee};
    if (pointee.is_volatile() && gs::is_value_type(pointee.get_kind())) {
        auto& pool{comp_.ctx().pool};
        auto& plain{*pool.with_const(*pool.strip_volatile(pointee), true)};
        return gir::value{builder_.emit_load(p.address, pointee), plain};
    }

    return gir::value{builder_.emit_load(p.address, pointee), pointee};
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

    // A `let mut` global is addressed where it lives, not through its read value
    if (const auto inst{as_inst(r)};
        inst && mod().tag_of(*inst) == tag::decl_val &&
        mod().decl_at(decl_id{mod().data_of(*inst).un.aux}).has(decl_flags::MUT)) {
        const auto use{TRY(analyze_decl_val(*inst, true))};
        if (const auto p{use.as_opt<place>()}) { return *p; }
    }

    const auto op{TRY(resolve(r, src))};
    if (const auto p{op.as_opt<place>()}) {
        if (!p->pending && !p->address.type) {
            return comp_.unsupported(u_, src, "an untyped `let mut`'s address");
        }

        return settle(*p);
    }

    if (const auto s{op.as_opt<slot_value>()}) {
        return place{.address    = s->address,
                     .pointee    = s->type,
                     .is_mutable = s->is_mutable,
                     .owned_copy = s->owned_copy};
    }

    if (const auto g{op.as_opt<global_ref>()}) { return global_place(g->symbol, *g->type, true); }
    if (const auto g{op.as_opt<global_const>()}) {
        return global_place(g->symbol, *g->type, false);
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

    const auto value{TRY([&] -> result<gir::value> {
        if (const auto built{to_entry ? entry_strings_.find(*inst) : entry_strings_.end()};
            built != entry_strings_.end()) {
            return built->second;
        }

        const auto here{builder_.get_segment()};
        if (!to_entry || !op.is<ip::index>() || here->get_id() == gir::segment_id{0}) {
            return as_runtime(op, src);
        }

        // A constant is built on entry too, so its slot is ready wherever the binding is read
        const bool was_splicing{builder_.is_splicing()};
        builder_.set_segment(*gir_fn_->get_segment(gir::segment_id{0}));
        builder_.set_splice(true);
        const auto back{gsl::finally([&] {
            builder_.set_segment(*here);
            builder_.set_splice(was_splicing);
        })};
        return as_runtime(op, src);
    }())};
    auto&      type{*value.type};
    auto       slot{spill(value, type, to_entry)};
    if (binding) {
        slot.owned_copy = mod().tag_of(*inst) == tag::param && op.is<gir::value>();
        // An immutable pointer or reference binding never changes, so its reads keep the value
        const bool unchanged{op.is<gir::value>() && gs::pointee_of(type)};
        values_.insert_or_assign(
            *inst,
            slot_value{.address    = slot.address,
                       .type       = &type,
                       .known      = comptime_value(op),
                       .origin     = unchanged ? stdx::option<gir::value>{value} : stdx::none,
                       .owned_copy = slot.owned_copy});
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

namespace {

// A pointer or reference whose pointee may be written: `mut?` isn't, since a call decides it
[[nodiscard]] auto writable_through(const gs::type& type) -> bool {
    return !type.is_constant() && !type.is_poly();
}

} // namespace

auto element_view(gs::context& ctx, gs::type& elem, bool constant) -> gs::type& {
    if (gs::pointee_of(elem) || elem.get_kind() == gs::type_kind::SLICE) { return elem; }
    return *ctx.pool.with_const(elem, constant);
}

auto analyzer::through_pointer(const place& base, const source& src, bool checked)
    -> result<place> {
    // The address is typed by what it points to, as every place's is
    auto& type{*base.pointee};
    if (const auto r{type.get_data().as_opt<gs::types::reference>()}) {
        const auto loaded{load(base)};
        return place{.address    = gir::value{loaded.data, r->underlying},
                     .pointee    = &r->underlying,
                     .is_mutable = writable_through(type)};
    }

    if (const auto p{type.get_data().as_opt<gs::types::pointer>()}) {
        const auto loaded{load(base)};
        if (checked) { TRY(check_not_null(loaded, src)); }
        return place{.address    = gir::value{loaded.data, p->underlying},
                     .pointee    = &p->underlying,
                     .is_mutable = writable_through(type)};
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

auto analyzer::immutable_view(gs::type& field) -> gs::type& {
    const auto array{field.get_data().as_opt<gs::types::array>()};
    if (!array || field.is_constant()) { return field; }
    return comp_.ctx().get_array(
        gs::types::mut::CONSTANT, array->null_terminated, array->len, array->underlying);
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
    if (is_bit_packed(type)) { return packed_place(base, index, field_type_at(type, index)); }
    if (const auto ut{type.get_data().as_opt<gs::types::union_t>()}) {
        auto& declared{ut->type_at(index)};
        auto& field_type{mutable_view(declared, base.is_mutable || base.owned_copy)};
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

    // A volatile struct's fields are volatile too
    auto& declared{*comp_.ctx().pool.with_volatile(
        type.get_data().as<gs::types::struct_t>().type_at(index), type.is_volatile())};

    // Through a `mut?` pointer an array field's elements read as immutable
    auto&      field_type{object.pointee->is_poly()
                              ? immutable_view(declared)
                              : mutable_view(declared, base.is_mutable || base.owned_copy)};
    const auto at{builder_.emit_get_element_ptr(base.address, {usize_constant(index)}, field_type)};
    return place{
        .address    = gir::value{at, field_type},
        .pointee    = &field_type,
        .is_mutable = base.is_mutable,
    };
}

auto analyzer::analyze_field_val(inst_id inst) -> result<operand> {
    auto       rec{record(inst)};
    const auto object_ref{*rec.ref_field()};
    const auto name{rec.str_field()};
    return field_value(object_ref, name, src_of(inst));
}

auto analyzer::field_value(ref object_ref, std::string_view name, const source& src, bool checked)
    -> result<operand> {
    const auto object{TRY(resolve(object_ref, src))};
    if (const auto m{object.as_opt<module_ref>()}) {
        return declaration_use(TRY(module_member(*m, name, src)), false, src);
    }

    // A closure's `.thunk` is its body, which takes the environment as its first argument
    if (const auto body{comp_.closure_body(type_of(object))}; body && name == "thunk") {
        return function_ref{&*body};
    }

    if (const auto pack{object.as_opt<pack_ref>()}) {
        if (name != "len") { return comp_.unsupported(u_, src, "a field of a pack"); }
        return operand{comp_.constants().intern(u64{packs_[pack->index].size()},
                                                builtin(gs::type_kind::USIZE))};
    }

    // A field through a compile-time address reads the memory it names
    if (const auto field{memory_field(object, name)}) { return *field; }
    if (const auto memory{memory_target(object)}) {
        const auto field{TRY(comptime_field_place(*memory, name, src))};
        return load_comptime(field.as<comptime_place>());
    }

    // A known address's field at compile time is the field of the value it holds
    if (is_comptime() && gs::pointee_of(type_of(object))) {
        if (const auto referent{comptime_referent(object)}) {
            return comptime_field(*referent, name, src);
        }
    }

    if (const auto index{comptime_value(object)}) {
        if (comp_.constants().is_type(*index)) {
            return declaration_use(
                TRY(member_of(comp_.constants().type_at(*index), name, src)), false, src);
        }

        // A runtime body reads a known string slice through a copy of it
        const auto& known{comp_.constants().value_at(*index)};
        const auto  slice_type{known.type};
        if (!is_comptime() && known.data.is<std::string>() && slice_type &&
            slice_type->get_kind() == gs::type_kind::SLICE) {
            // Building the slice interns constants, which may move `known`
            const auto built{TRY(as_runtime(object, src))};
            const auto copy{spill(built, *slice_type, false)};
            const auto field{TRY(field_place(copy, name, src))};
            return operand{TRY(read(field, src))};
        }

        return comptime_field(*index, name, src);
    }

    if (const auto g{object.as_opt<global_const>()}; g && is_comptime()) {
        return comptime_field(g->value, name, src);
    }

    if (object.is<function_ref>()) { return comp_.unsupported(u_, src, "a field of a function"); }
    TRY(require_runtime(src));

    // A bit-packed value's field is its bits, read without storing the value
    if (const auto v{object.as_opt<gir::value>()}; v && is_bit_packed(type_of(object))) {
        auto&      type{type_of(object)};
        const auto index{TRY(field_index(type, name, src))};
        return operand{packed_extract(*v, type, index, field_type_at(type, index))};
    }

    const auto object_place{TRY(address_of(object_ref, src))};
    const auto base{TRY(through_pointer(object_place, src, checked))};
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
    // A tagged union is read where it is used, so its tag is checked in place
    if (is_tagged_union(*field.pointee)) {
        const auto settled{settle(field)};
        return slot_value{
            .address = settled.address, .type = settled.pointee, .is_mutable = settled.is_mutable};
    }

    // An unchecked read, like `@field`'s, doesn't check a union's active field
    if (!checked) { return operand{load(settle(field))}; }
    return operand{TRY(read(field, src))};
}

auto analyzer::analyze_field_ptr(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  object_ref{*rec.ref_field()};
    const auto  name{rec.str_field()};
    const bool  assigned{(rec.u32_field() & flags::ASSIGNED) != 0};
    const auto  object{TRY(resolve(object_ref, src))};
    // `T.member` and `module.member` name the member's own storage
    const auto index{comptime_value(object)};
    const auto m{object.as_opt<module_ref>()};
    if (m || (index && comp_.constants().is_type(*index))) {
        const auto member{TRY(m ? module_member(*m, name, src)
                                : member_of(comp_.constants().type_at(*index), name, src))};
        if (is_comptime()) {
            if (const auto g{member.as_opt<global_const>()}) { return g->value; }
            return member;
        }

        if (const auto g{member.as_opt<global_ref>()}) {
            return global_place(g->symbol, *g->type, true);
        }

        if (const auto g{member.as_opt<global_const>()}) {
            return global_place(g->symbol, *g->type, false);
        }

        return member;
    }

    // A field through a compile-time address is a field of the memory it names
    if (const auto field{memory_field(object, name)}) { return *field; }
    if (const auto memory{memory_target(object)}) {
        return comptime_field_place(*memory, name, src);
    }

    // A compile-time reference's field is the field of what it holds
    if (const auto referent{comptime_referent(object)}; referent && !assigned) {
        return comptime_field(*referent, name, src);
    }

    // A field of a compile-time value is known so reading it needs no storage
    if (const auto known{comptime_value(object)}; known && !assigned) {
        return comptime_field(*known, name, src);
    }

    if (const auto g{object.as_opt<global_const>()}; g && is_comptime() && !assigned) {
        return comptime_field(g->value, name, src);
    }

    // A runtime body folding a condition can't read a runtime local
    if (is_comptime() && gir_fn_ && (object.is<place>() || object.is<slot_value>())) {
        return comp_.needs_runtime(u_, src);
    }

    if (const auto memory{object.as_opt<comptime_place>()}) {
        return comptime_field_place(*memory, name, src);
    }

    if (is_comptime()) { return comp_.unsupported(u_, src, "a field of comptime memory"); }

    const auto base{TRY(address_of(object_ref, src))};

    // An array's length is its type's so it names no storage
    if (const auto arr{base.pointee->get_data().as_opt<gs::types::array>()};
        arr && name == "len" && !assigned) {
        return comp_.constants().intern(u64{arr->len}, builtin(gs::type_kind::USIZE));
    }

    // An array's `.ptr` is the address of its first element
    if (const auto arr{base.pointee->get_data().as_opt<gs::types::array>()};
        arr && name == "ptr" && !assigned) {
        auto& ptr_type{comp_.ctx().get_pointer(base.pointee->get_key().get_mut(), arr->underlying)};
        const auto at{builder_.emit_get_element_ptr(
            base.address, {usize_constant(ARRAY_PTR_FIELD)}, ptr_type)};
        return operand{gir::value{at, ptr_type}};
    }

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

auto analyzer::elem_place(const place& object, ref index_ref, const source& src, bool written)
    -> result<place> {
    // A pointer to one element indexes the elements from where it points, the index read first
    const auto pointer{object.pointee->get_data().as_opt<gs::types::pointer>()};
    if (pointer && !pointer->underlying.get_data().is<gs::types::array>() &&
        !pointer->underlying.get_data().is<gs::types::slice>()) {
        const auto index{TRY(index_operand(index_ref, src))};
        const auto base{TRY(through_pointer(object, src))};
        auto&      elem{*base.pointee};
        const auto at{builder_.emit_get_element_ptr(
            gir::value{base.address.data, *object.pointee}, {index}, elem)};
        return place{.address    = gir::value{at, elem},
                     .pointee    = &elem,
                     .is_mutable = writable_through(*object.pointee)};
    }

    const auto base{TRY(through_pointer(object, src))};
    const auto index{TRY(index_operand(index_ref, src))};
    auto&      type{*base.pointee};
    auto&      ctx{comp_.ctx()};
    const bool safety{runtime_safety()};

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

    // A lane is read from and written into the vector loaded after its index
    if (const auto vec{gs::vector_of(type)}) {
        auto&                    usize_type{builtin(gs::type_kind::USIZE)};
        stdx::option<gir::value> loaded;
        if (!written) { loaded.emplace(builder_.emit_load(base.address, type), type); }
        if (safety && !index.data.is<ip::index>()) {
            auto&      bool_type{builtin(gs::type_kind::BOOL)};
            const auto as_usize{
                index.type && gs::is_same_unqualified(*index.type, usize_type)
                    ? index
                    : gir::value{
                          builder_.emit_cast(gir::instruction_kind::INT_CAST, index, usize_type),
                          usize_type}};
            const auto in_bounds{builder_.emit_binary(
                gir::instruction_kind::LT, as_usize, usize_constant(vec->len), bool_type)};
            TRY(guard(gir::value{in_bounds, bool_type}, "index out of bounds", src));
        }

        if (!loaded) { loaded.emplace(builder_.emit_load(base.address, type), type); }
        return place{.address    = base.address,
                     .pointee    = &vec->underlying,
                     .is_mutable = base.is_mutable,
                     .lane       = vector_lane_at{.index = index, .vector = *loaded}};
    }

    if (const auto arr{type.get_data().as_opt<gs::types::array>()}) {
        if (safety) {
            const auto bound{
                constant(comp_.constants().intern(u64{arr->len}, index_type), index_type)};
            TRY(bounds_check(compared(), bound, arr->null_terminated, src));
        }

        auto&      elem{element_view(ctx, arr->underlying, type.is_constant())};
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

        auto&      elem{element_view(ctx, s->underlying, type.is_constant())};
        const auto at{builder_.emit_get_element_ptr(gir::value{ptr, ptr_type}, {index}, elem)};
        return place{.address    = gir::value{at, elem},
                     .pointee    = &elem,
                     .is_mutable = writable_through(type)};
    }

    return comp_.unsupported(u_, src, fmt::format("indexing a `{}`", type.to_string()));
}

auto analyzer::analyze_elem_val(inst_id inst) -> result<operand> {
    const auto& d{mod().data_of(inst)};
    const auto& src{src_of(inst)};
    const auto  object{TRY(resolve(d.bin.lhs, src))};
    if (const auto pack{object.as_opt<pack_ref>()}) { return pack_elem(*pack, d.bin.rhs, src); }
    if (const auto element{TRY(memory_element(object, d.bin.rhs, src))}) {
        return load_comptime(*element);
    }

    // An array kept in comptime memory reads its element there
    if (const auto memory{object.as_opt<comptime_place>()}) {
        const auto element{TRY(comptime_elem_place(*memory, d.bin.rhs, src))};
        return load_comptime(element.as<comptime_place>());
    }

    if (!object.is<module_ref>() && vector_read(type_of(object))) {
        return vector_lane(object, d.bin.rhs, src);
    }

    // A runtime body reads a string's bytes where the string is stored
    const auto index{comptime_value(object)};
    const bool text{index && !comp_.constants().is_type(*index) &&
                    comp_.constants().value_at(*index).data.is<std::string>()};
    if (index && (!text || is_comptime())) {
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
    const bool  mutated{(rec.u32_field() & flags::MUTATED) != 0};
    const auto  object{TRY(resolve(object_ref, src))};

    // A pack's element is the value it holds, a slice reached in its slot
    if (const auto pack{object.as_opt<pack_ref>()}) {
        const auto element{TRY(pack_elem(*pack, index_ref, src))};
        if (const auto slot{element.as_opt<slot_value>()}; slot && !is_comptime()) {
            return operand{
                place{.address = slot->address, .pointee = slot->type, .is_mutable = false}};
        }

        return element;
    }

    // An element through a compile-time slice or address is an element of the memory it names
    if (const auto element{TRY(memory_element(object, index_ref, src))}) {
        return operand{*element};
    }
    if (const auto memory{memory_target(object)}) {
        return comptime_elem_place(*memory, index_ref, src);
    }

    // An element of a known value read at compile time is that element
    if (is_comptime() || holds_type(type_of(object))) {
        const auto global{object.as_opt<global_const>()};
        const auto known{global ? stdx::option<ip::index>{global->value}
                                : comptime_value(held_address(object))};
        const auto at{known ? comptime_value(TRY(resolve_value(index_ref, src))) : stdx::none};
        if (known && at) { return comptime_elem(*known, *at, src); }
    }

    // A runtime body folding a condition can't read a runtime local
    if (is_comptime() && gir_fn_ && (object.is<place>() || object.is<slot_value>())) {
        return comp_.needs_runtime(u_, src);
    }

    if (const auto memory{object.as_opt<comptime_place>()}) {
        // A runtime index reads the element from a copy of the whole value
        const auto at{TRY(resolve_value(index_ref, src))};
        if (gir_fn_ && !is_comptime() && !mutated && !comptime_value(at)) {
            const auto copy{TRY(comptime_copy(*memory, src))};
            return operand{TRY(elem_place(copy, index_ref, src))};
        }

        return comptime_elem_place(*memory, index_ref, src);
    }

    if (is_comptime()) { return comp_.unsupported(u_, src, "an element of comptime memory"); }

    const auto object_place{TRY(address_of(object_ref, src))};
    const auto elem{TRY(elem_place(object_place, index_ref, src, mutated))};
    return operand{elem};
}

auto analyzer::analyze_ref_of(inst_id inst, bool is_pointer) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  operand_ref{*rec.ref_field()};
    const auto  bits{rec.u32_field()};

    // `&mut?` takes the mutability of what it refers to, so it never requires a mutable place
    const bool is_mut{(bits & flags::MUT) != 0 && (bits & flags::POLY) == 0};
    const auto target{TRY(resolve(operand_ref, src))};
    auto&      ctx{comp_.ctx()};
    const auto mut{(bits & flags::POLY) != 0 ? gs::types::mut::POLY
                   : is_mut                  ? gs::types::mut::MUTABLE
                                             : gs::types::mut::CONSTANT};

    // An errdefer's `|&e|` or `|^e|` is the residual's own slot, kept in a slot of its own
    if (const auto residual{as_inst(operand_ref)};
        residual && !is_comptime() &&
        (mod().tag_of(*residual) == tag::failure_residual ||
         mod().tag_of(*residual) == tag::residual)) {
        if (const auto slot{target.as_opt<slot_value>()}) {
            auto&            alias_type{is_pointer ? ctx.get_pointer(mut, *slot->type)
                                                   : ctx.get_reference(mut, *slot->type)};
            const gir::value alias{slot->address.data, alias_type};
            const auto       kept{spill(alias, alias_type, false)};
            return slot_value{.address = kept.address, .type = &alias_type};
        }
    }

    // `^T` of a type is the pointer type
    if (const auto index{comptime_value(target)}; index && comp_.constants().is_type(*index)) {
        auto& pointee{comp_.constants().type_at(*index)};
        return interned_type(is_pointer ? ctx.get_pointer(mut, pointee)
                                        : ctx.get_reference(mut, pointee));
    }

    // A global's address is a constant, which a global's initializer may hold
    if (const auto global{target.as_opt<global_ref>()}; global && is_comptime() && !gir_fn_) {
        auto& pointee{*global->type};
        auto& address_type{is_pointer ? ctx.get_pointer(mut, pointee)
                                      : ctx.get_reference(mut, pointee)};
        return comp_.constants().intern(
            ip::address_value{.symbol = std::string{global->symbol}, .pointee = {}}, address_type);
    }

    // `^.{ ... }` is an array backing the slice it is used as
    if (const auto init{target.as_opt<anon_init>()}; init && is_pointer) {
        return slice_literal{init->inst};
    }

    // An address of compile-time memory names it, so writes through it reach the memory
    if (const auto memory{target.as_opt<comptime_place>()}; memory && is_comptime()) {
        auto& pointee{type_of(target)};
        auto& address_type{is_pointer ? ctx.get_pointer(mut, pointee)
                                      : ctx.get_reference(mut, pointee)};
        return operand{memory_address(*memory, address_type)};
    }

    // A function's address at compile time is the function, which a `^fn` holds as its value
    if (target.is<function_ref>() && !is_mut && (is_comptime() || is_pointer)) { return target; }

    // `&f` of a function item borrows a copy of its code address
    if (const auto fn{target.as_opt<function_ref>()}; fn && !is_mut && !is_comptime()) {
        auto&      thin{*fn->fn->type};
        const auto code{TRY(as_runtime(target, src))};
        const auto slot{spill(code, thin, false)};
        auto&      ref_type{is_pointer ? ctx.get_pointer(mut, thin) : ctx.get_reference(mut, thin)};
        const gir::value address{builder_.emit_address_of(slot.address, ref_type), ref_type};
        return operand{address};
    }

    // A known value's address at compile time holds the value, which nothing may write through
    if (const auto known{comptime_value(target)}; known && is_comptime() && !is_mut) {
        auto& pointee{type_of(target)};
        auto& address_type{is_pointer ? ctx.get_pointer(mut, pointee)
                                      : ctx.get_reference(mut, pointee)};
        return comp_.constants().intern(ip::address_value{.symbol = {}, .pointee = {*known}},
                                        address_type);
    }

    if (is_comptime()) { return comp_.unsupported(u_, src, "an address at compile time"); }

    auto& target_type{type_of(target)};
    if (target_type.is_poison()) { return stdx::err{stop::POISON}; }
    if (target.is<function_ref>() || target_type.get_kind() == gs::type_kind::REFERENCE) {
        return comp_.unsupported(u_, src, "the address of a function or reference");
    }

    const auto slot{TRY(address_of(operand_ref, src))};
    if (slot.lane) {
        return comp_.report(u_,
                            src,
                            gs::error::ILLEGAL_VECTOR_LANE_ADDRESS,
                            fmt::format("cannot take {} a vector lane; read it into a local first",
                                        is_pointer ? "the address of" : "a reference to"));
    }

    if (slot.packed) {
        return comp_.report(u_,
                            src,
                            gs::error::ILLEGAL_PACKED_FIELD_ADDRESS,
                            fmt::format("cannot take {} a field of a 'packed struct'; copy it into "
                                        "a local first",
                                        is_pointer ? "the address of" : "a reference to"));
    }

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
    return place{.address = value, .pointee = referent.get(), .is_mutable = writable_through(type)};
}

auto analyzer::comptime_deref(const operand& ptr) -> stdx::option<operand> {
    auto&      pool{comp_.constants()};
    auto&      type{type_of(ptr)};
    const auto known{comptime_value(ptr)};

    // `*s` of a known slice is the array of its elements
    if (const auto slice{type.get_data().as_opt<gs::types::slice>()}) {
        std::vector<stdx::option<ip::index>> elements;
        if (const auto view{view_of(ptr)}) {
            for (u64 i{0}; i < view->len; ++i) {
                auto element{view->array};
                element.path.emplace_back(
                    comptime_step{static_cast<u32>(view->offset + i), view->elem});
                elements.emplace_back(comptime_value(load_comptime(element)));
            }
        } else if (!known) {
            return stdx::none;
        } else if (const auto text{pool.value_at(*known).data.as_opt<std::string>()}) {
            const std::string bytes{*text};
            for (const char c : bytes) {
                elements.emplace_back(pool.intern(u64{static_cast<u8>(c)}, slice->underlying));
            }
        } else if (const auto agg{pool.value_at(*known).data.as_opt<ip::aggregate_value>()}) {
            elements = agg->elements;
        } else {
            return stdx::none;
        }

        auto& array{comp_.ctx().get_array(
            gs::types::mut::CONSTANT, false, elements.size(), slice->underlying)};
        return operand{pool.intern(ip::aggregate_value{std::move(elements)}, array)};
    }

    // An `undefined` pointer reads an `undefined` value
    const auto pointee{gs::pointee_of(type)};
    if (known && pointee && !pool.is_type(*known) &&
        pool.value_at(*known).data.is<ip::undefined_value>()) {
        return operand{pool.intern(ip::undefined_value{}, *pointee)};
    }

    return stdx::none;
}

auto analyzer::analyze_deref(inst_id inst) -> result<operand> {
    const auto& d{mod().data_of(inst)};
    const auto& src{src_of(inst)};
    const auto  ptr{TRY(resolve_value(d.un.operand, src))};
    const bool  as_place{(d.un.aux & flags::DEREF_PLACE) != 0};

    // A `^fn` constant holds its function, which it dereferences to
    if (const auto index{comptime_value(ptr)}; index && !as_place && comp_.plain_function(*index)) {
        return ptr;
    }

    // A compile-time address reads or writes the memory it names
    if (const auto memory{memory_place(ptr)}) {
        if (as_place) { return operand{*memory}; }
        return load_comptime(*memory);
    }

    if (const auto referent{comptime_referent(ptr)}; referent && !as_place && is_comptime()) {
        return *referent;
    }

    if (is_comptime() && !as_place) {
        if (const auto value{comptime_deref(ptr)}) { return *value; }
    }

    if (is_comptime()) { return comp_.unsupported(u_, src, "a dereference at compile time"); }
    auto& type{type_of(ptr)};
    if (type.get_kind() == gs::type_kind::SLICE) {
        return deref_slice(d.un.operand, ptr, as_place, src);
    }

    if (gs::is_fat_callable(type)) {
        return comp_.unsupported(u_, src, "dereferencing a callable");
    }

    const auto target{TRY(pointer_place(ptr, src))};
    if (as_place) { return target; }
    // An aggregate is read where it lies, so a field or element of it is addressed in place
    if (gs::is_structural(target.pointee->get_kind())) {
        return slot_value{
            .address = target.address, .type = target.pointee, .is_mutable = target.is_mutable};
    }

    return operand{load(target)};
}

auto analyzer::deref_slice(ref operand_ref, const operand& slice, bool as_place, const source& src)
    -> result<operand> {
    // `*s[a..b]` is the array the range's known length spans
    const auto inst{as_inst(operand_ref)};
    if (!inst || mod().tag_of(*inst) != tag::slice) {
        return comp_.unsupported(u_, src, "dereferencing a slice of unknown length");
    }

    auto rec{record(*inst)};
    rec.skip_ref();
    const auto lo_ref{rec.ref_field()};
    const auto hi_ref{rec.ref_field()};
    const bool inclusive{(rec.u32_field() & flags::INCLUSIVE) != 0};
    const auto known_bound{[&](stdx::option<ref> r) -> stdx::option<i128> {
        if (!r) { return i128{0}; }
        const auto bound{resolve(*r, src)};
        const auto index{bound ? comptime_value(*bound) : stdx::option<ip::index>{}};
        const auto exact{index ? int_of(comp_.constants().value_at(*index)) : stdx::none};
        return exact ? exact->to_i128() : stdx::none;
    }};
    const auto lo{known_bound(lo_ref)};
    const auto hi{hi_ref ? known_bound(hi_ref) : stdx::none};
    if (!lo || !hi || *hi + (inclusive ? 1 : 0) < *lo) {
        return comp_.unsupported(u_, src, "dereferencing a slice of unknown length");
    }

    auto&       ctx{comp_.ctx()};
    auto&       slice_type{type_of(slice)};
    const auto& s{slice_type.get_data().as<gs::types::slice>()};
    const auto  mut{slice_type.is_constant() ? gs::types::mut::CONSTANT : gs::types::mut::MUTABLE};
    const auto  len{static_cast<u64>(*hi + (inclusive ? 1 : 0) - *lo)};
    auto&       array{ctx.get_array(mut, false, len, s.underlying)};
    auto&       array_ptr{ctx.get_pointer(mut, array)};

    // The slice is spilled and its pointer read as the array's address
    const auto slot{spill(TRY(as_runtime(slice, src)), slice_type, false).address};
    const auto ptr_at{
        builder_.emit_get_element_ptr(slot, {usize_constant(SLICE_PTR_FIELD)}, array_ptr)};
    const gir::value address{builder_.emit_load(gir::value{ptr_at, array_ptr}, array_ptr),
                             array_ptr};
    const place target{.address = address, .pointee = &array, .is_mutable = !array.is_constant()};
    if (as_place) { return target; }
    return slot_value{.address = address, .type = &array, .is_mutable = target.is_mutable};
}

auto analyzer::operand_address(const operand& op, const source& src) -> result<place> {
    TRY(require_runtime(src));
    if (const auto s{op.as_opt<slot_value>()}) {
        return place{.address = s->address, .pointee = s->type, .is_mutable = s->is_mutable};
    }

    if (const auto p{op.as_opt<place>()}) { return settle(*p); }
    if (const auto g{op.as_opt<global_ref>()}) { return global_place(g->symbol, *g->type, true); }
    if (const auto g{op.as_opt<global_const>()}) {
        return global_place(g->symbol, *g->type, false);
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
        agg && type.get_kind() == gs::type_kind::SLICE && name == "len") {
        return pool.intern(u64{agg->elements.size()}, builtin(gs::type_kind::USIZE));
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

    // A known slice's address holds its elements, so equal slices share one
    if (const auto slice{type.get_data().as_opt<gs::types::slice>()}; slice && name == "ptr") {
        auto& ptr_type{comp_.ctx().get_pointer(gs::types::mut::CONSTANT, slice->underlying)};
        std::vector<ip::index> pointee;
        if (const auto text{value.data.as_opt<std::string>()}) {
            const std::string bytes{*text};
            for (const char c : bytes) {
                pointee.emplace_back(pool.intern(u64{static_cast<u8>(c)}, slice->underlying));
            }
        } else if (const auto agg{value.data.as_opt<ip::aggregate_value>()}) {
            for (const auto element : agg->elements) {
                if (!element) {
                    return comp_.unsupported(u_, src, "the address of undefined elements");
                }
                pointee.emplace_back(*element);
            }
        }

        return pool.intern(ip::address_value{.symbol = {}, .pointee = std::move(pointee)},
                           ptr_type);
    }

    return comp_.unsupported(u_, src, fmt::format("a field of a comptime `{}`", type.to_string()));
}

auto element_type_of(gs::type& t) -> stdx::option<gs::type&> {
    if (const auto arr{t.get_data().as_opt<gs::types::array>()}) { return arr->underlying; }
    if (const auto slice{t.get_data().as_opt<gs::types::slice>()}) { return slice->underlying; }
    if (const auto vec{gs::vector_of(t)}) { return vec->underlying; }
    return stdx::none;
}

auto analyzer::pack_elem(pack_ref pack, ref at_ref, const source& src) -> result<operand> {
    const auto at{comptime_value(TRY(resolve_value(at_ref, src)))};
    const auto exact{at ? int_of(comp_.constants().value_at(*at)) : stdx::none};
    const auto small{exact ? exact->to_i128() : stdx::none};
    if (!small) { return comp_.unsupported(u_, src, "a pack indexed at runtime"); }
    const auto& elements{packs_[pack.index]};
    if (*small < 0 || *small >= static_cast<i128>(elements.size())) {
        return comp_.report(u_, src, gs::error::COMPTIME_EVALUATION_FAILED, "index out of bounds");
    }

    // A runtime value read out of a pack is copied through a slot of its own
    const auto& element{elements[static_cast<usize>(*small)]};
    const auto  value{element.as_opt<gir::value>()};
    if (!value || is_comptime()) { return element; }
    auto&      type{*value->type};
    const auto slot{builder_.emit_alloca(type)};
    builder_.emit_store(gir::value{slot, type}, *value);
    return operand{gir::value{builder_.emit_load(gir::value{slot, type}, type), type}};
}

auto analyzer::comptime_elem(ip::index object, ip::index at, const source& src) -> result<operand> {
    auto&       pool{comp_.constants()};
    const auto& value{pool.value_at(object)};
    const auto  agg{value.data.as_opt<ip::aggregate_value>()};
    const auto  text{value.data.as_opt<std::string>()};
    const auto  index{int_of(pool.value_at(at))};
    const auto  elem_type{value.type ? element_type_of(*value.type) : stdx::none};
    if ((!agg && !text) || !index || !elem_type) {
        return comp_.unsupported(u_, src, "indexing this comptime value");
    }

    const auto small{index->to_i128()};
    const auto count{agg ? agg->elements.size() : text->size()};

    // A sentinel-terminated array's sentinel sits just past its elements
    const auto arr{value.type ? value.type->get_data().as_opt<gs::types::array>() : stdx::none};
    if (small && *small == static_cast<i128>(count) && arr && arr->null_terminated) {
        return intern_int(pool, big_int{i128{0}}, *elem_type);
    }

    if (!small || *small < 0 || *small >= static_cast<i128>(count)) {
        return comp_.report(u_, src, gs::error::COMPTIME_EVALUATION_FAILED, "index out of bounds");
    }

    // A string's elements are its bytes
    const auto at_index{static_cast<usize>(*small)};
    if (text) {
        return intern_int(
            pool, big_int{static_cast<u128>(static_cast<u8>((*text)[at_index]))}, *elem_type);
    }
    const auto element{agg->elements[at_index]};
    return element.value_or(pool.intern(ip::undefined_value{}, *elem_type));
}

} // namespace ghoti::uir::sema
