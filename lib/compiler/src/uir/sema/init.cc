#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
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
#include "support/string_utils.hh"

namespace ghoti::uir::sema {

namespace {

// One `.name = value` entry of a struct initializer
struct named_value {
    std::string_view name;
    ref              value;
};

[[nodiscard]] auto named_values(record_reader& rec) -> std::vector<named_value> {
    std::vector<named_value> out;
    const auto               count{rec.count()};
    for (u32 i{0}; i < count; ++i) {
        const auto name{rec.str_field()};
        out.emplace_back<named_value>({.name = name, .value = *rec.ref_field()});
    }

    return out;
}

} // namespace

auto analyzer::analyze_init_struct(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  type_ref{rec.ref_field()};
    if (!type_ref) { return anon_init{inst}; }
    return init_as(inst, *TRY(resolve_type(*type_ref, src)), src);
}

auto analyzer::analyze_init_array(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  type_ref{rec.ref_field()};
    if (!type_ref) { return anon_init{inst}; }
    return init_as(inst, *TRY(resolve_type(*type_ref, src)), src);
}

auto analyzer::analyze_array_lit(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  len_ref{rec.ref_field()};
    auto&       elem{*TRY(resolve_type(*rec.ref_field(), src))};
    const auto  bits{rec.u32_field()};
    const auto  items{rec.refs_field()};
    if ((bits & flags::POLY) != 0) { return comp_.unsupported(u_, src, "a `mut?` type"); }
    const usize len{len_ref ? TRY(array_length(*len_ref, src)) : items.size()};
    const auto  mut{(bits & flags::MUT) != 0 ? gs::types::mut::MUTABLE : gs::types::mut::CONSTANT};
    auto&       type{comp_.ctx().get_array(mut, (bits & flags::SENTINEL) != 0, len, elem)};
    return init_as(inst, type, src);
}

auto analyzer::init_as(inst_id inst, gs::type& type, const source& src) -> result<operand> {
    TRY(comp_.require_fields(u_, src, type));
    switch (type.get_kind()) {
    case gs::type_kind::STRUCT:
        if (mod().tag_of(inst) != tag::init_struct) { break; }
        return init_struct(inst, type, src);
    case gs::type_kind::UNION:
        if (mod().tag_of(inst) != tag::init_struct) { break; }
        return init_union(inst, type, src);
    case gs::type_kind::ARRAY: return init_array(inst, type, src);
    default:                   break;
    }

    return comp_.unsupported(
        u_, src, fmt::format("an initializer of a `{}`", comp_.ctx().type_display_name(type)));
}

auto analyzer::init_struct(inst_id inst, gs::type& type, const source& src) -> result<operand> {
    auto rec{record(inst)};
    rec.skip_ref();
    const auto  entries{named_values(rec)};
    const auto& st{type.get_data().as<gs::types::struct_t>()};

    // Every field is named once, exists, and is given or has a default
    ankerl::unordered_dense::set<std::string_view> seen;
    std::vector<std::string_view>                  duplicates;
    std::vector<std::string_view>                  unknown;
    std::vector<usize>                             indices;
    for (const auto& entry : entries) {
        if (!seen.insert(entry.name).second) { duplicates.emplace_back(entry.name); }
        const auto found{std::ranges::find(st.field_names, entry.name)};
        if (found == st.field_names.end()) {
            unknown.emplace_back(entry.name);
        } else {
            indices.emplace_back(static_cast<usize>(found - st.field_names.begin()));
        }
    }

    if (!duplicates.empty()) {
        return comp_.report(u_,
                            src,
                            gs::error::DUPLICATE_FIELD,
                            fmt::format("Struct initializer contains duplicate field{}: {}",
                                        string_utils::plural_suffix(duplicates.size()),
                                        fmt::join(duplicates, ", ")));
    }

    const auto                    defaults{TRY(field_defaults(type, src))};
    std::vector<std::string_view> missing;
    for (usize i{0}; i < st.field_names.size(); ++i) {
        if (!seen.contains(st.field_names[i]) && defaults[i].empty()) {
            missing.emplace_back(st.field_names[i]);
        }
    }

    if (!missing.empty()) {
        return comp_.report(u_,
                            src,
                            gs::error::MISSING_FIELD,
                            fmt::format("Struct initializer missing required field{}: {}",
                                        string_utils::plural_suffix(missing.size()),
                                        fmt::join(missing, ", ")));
    }

    if (!unknown.empty()) {
        return comp_.report(u_,
                            src,
                            gs::error::UNKNOWN_FIELD,
                            fmt::format("Struct initializer contains unknown field{}: {}",
                                        string_utils::plural_suffix(unknown.size()),
                                        fmt::join(unknown, ", ")));
    }

    const auto field_value{[&](usize index, ref value_ref) -> result<operand> {
        const auto value{TRY(resolve_value(value_ref, src))};
        return coerce_to(value, st.type_at(index), src, {.what = use_site::kind::STORE});
    }};

    if (is_comptime()) {
        std::vector<stdx::option<ip::index>> elements(st.fields.size());
        for (usize i{0}; i < entries.size(); ++i) {
            const auto value{TRY(field_value(indices[i], entries[i].value))};
            elements[indices[i]] = TRY(require_comptime(value, src));
        }

        for (usize i{0}; i < st.fields.size(); ++i) {
            if (elements[i] || defaults[i].empty()) { continue; }
            const auto value{TRY(field_default(type, i, defaults[i], src))};
            elements[i] = TRY(require_comptime(value, src));
        }

        return comp_.constants().intern(ip::aggregate_value{std::move(elements)}, type);
    }

    const auto slot{builder_.emit_alloca(type)};
    for (usize i{0}; i < entries.size(); ++i) {
        auto& field_type{st.type_at(indices[i])};

        // A `type` field holds no runtime value
        if (field_type.get_kind() == gs::type_kind::TYPE) { continue; }
        const auto at{builder_.emit_get_element_ptr(
            gir::value{slot, type}, {usize_constant(indices[i])}, field_type)};
        const auto coerced{TRY(field_value(indices[i], entries[i].value))};
        const auto value{TRY(as_runtime(coerced, src))};
        builder_.emit_store(gir::value{at, field_type}, value).is_initializer = true;
    }

    for (usize i{0}; i < st.fields.size(); ++i) {
        if (seen.contains(st.field_names[i]) || defaults[i].empty()) { continue; }
        auto&      field_type{st.type_at(i)};
        const auto at{
            builder_.emit_get_element_ptr(gir::value{slot, type}, {usize_constant(i)}, field_type)};
        const auto coerced{TRY(field_default(type, i, defaults[i], src))};
        const auto value{TRY(as_runtime(coerced, src))};
        builder_.emit_store(gir::value{at, field_type}, value).is_initializer = true;
    }

    return gir::value{builder_.emit_load(gir::value{slot, type}, type), type};
}

auto analyzer::field_defaults(gs::type& type, const source& src)
    -> result<std::vector<gsl::span<const inst_id>>> {
    const auto n{comp_.nominal_of(type)};
    if (!n) { return comp_.unsupported(u_, src, "an initializer of a type UIR didn't declare"); }
    const auto&   m{*n->owner->uir};
    record_reader rec{m, m.data_of(n->inst).pl.extra};
    rec.skip_u32();
    const auto captures{rec.count()};
    for (u32 i{0}; i < captures; ++i) { rec.skip_ref().skip_u32().skip_str(); }

    std::vector<gsl::span<const inst_id>> defaults;
    const auto                            count{rec.count()};
    for (u32 i{0}; i < count; ++i) {
        rec.skip_str().skip_u32().skip_body();
        defaults.emplace_back(rec.body_field());
        rec.skip_body();
    }

    return defaults;
}

auto analyzer::field_default(gs::type&                type,
                             usize                    index,
                             gsl::span<const inst_id> body,
                             const source&            src) -> result<operand> {
    const auto n{comp_.nominal_of(type)};
    if (n->owner->index != u_.index) {
        return comp_.unsupported(u_, src, "a field default declared in another module");
    }

    const auto& st{type.get_data().as<gs::types::struct_t>()};
    const auto  out{TRY(analyze_body(body))};
    if (!out.yielded || jump_) { return comp_.unsupported(u_, src, "a field default that leaves"); }
    return coerce_to(*out.yielded, st.type_at(index), src, {.what = use_site::kind::STORE});
}

auto analyzer::init_union(inst_id inst, gs::type& type, const source& src) -> result<operand> {
    auto rec{record(inst)};
    rec.skip_ref();
    const auto  entries{named_values(rec)};
    const auto& ut{type.get_data().as<gs::types::union_t>()};
    if (entries.size() != 1) {
        return comp_.report(
            u_, src, gs::error::TYPE_MISMATCH, "A union initializer must set exactly one field");
    }

    const auto index{TRY(field_index(type, entries.front().name, src))};
    auto&      field_type{ut.type_at(index)};
    const auto given{TRY(resolve_value(entries.front().value, src))};
    const auto value{TRY(coerce_to(given, field_type, src, {.what = use_site::kind::STORE}))};
    if (is_comptime()) {
        const auto payload{TRY(require_comptime(value, src))};
        return comp_.constants().intern(
            ip::union_value{.active_field = std::string{entries.front().name},
                            .payload      = {payload}},
            type);
    }

    const auto slot{builder_.emit_alloca(type)};
    if (ut.is_untagged) {
        builder_.emit_store(gir::value{slot, field_type}, TRY(as_runtime(value, src)));
        return gir::value{builder_.emit_load(gir::value{slot, type}, type), type};
    }

    auto&      i32_type{comp_.ctx().get_int(32, true)};
    const auto tag_at{builder_.emit_get_element_ptr(
        gir::value{slot, type}, {usize_constant(UNION_TAG_FIELD)}, i32_type)};
    builder_
        .emit_store(gir::value{tag_at, i32_type},
                    constant(comp_.constants().intern(static_cast<i64>(index), i32_type), i32_type))
        .is_initializer = true;
    const auto payload_at{builder_.emit_get_element_ptr(
        gir::value{slot, type}, {usize_constant(UNION_PAYLOAD_FIELD)}, field_type)};
    builder_.emit_store(gir::value{payload_at, field_type}, TRY(as_runtime(value, src)))
        .is_initializer = true;
    return gir::value{builder_.emit_load(gir::value{slot, type}, type), type};
}

auto analyzer::init_array(inst_id inst, gs::type& type, const source& src) -> result<operand> {
    auto rec{record(inst)};

    // `T{ a, b }` and `.{ a, b }` list their values, `[N]T{ a, b }` its items
    const auto values{[&] -> stdx::option<gsl::span<const ref>> {
        switch (mod().tag_of(inst)) {
        case tag::init_array: rec.skip_ref(); return rec.refs_field();
        case tag::array_lit:  rec.skip_ref().skip_ref().skip_u32(); return rec.refs_field();
        default: return stdx::none;
        }
    }()};
    if (!values) { return comp_.unsupported(u_, src, "an array initialized by field names"); }
    const auto& arr{type.get_data().as<gs::types::array>()};
    auto&       elem{arr.underlying};
    if (values->size() != arr.len) {
        return comp_.report(u_,
                            src,
                            gs::error::TYPE_MISMATCH,
                            fmt::format("Expected {} array element{}, found {}",
                                        arr.len,
                                        string_utils::plural_suffix(arr.len),
                                        values->size()));
    }

    if (is_comptime()) {
        std::vector<stdx::option<ip::index>> elements;
        for (const auto value_ref : *values) {
            const auto given{TRY(resolve_value(value_ref, src))};
            const auto value{TRY(coerce_to(given, elem, src, {.what = use_site::kind::STORE}))};
            elements.emplace_back(TRY(require_comptime(value, src)));
        }

        return comp_.constants().intern(ip::aggregate_value{std::move(elements)}, type);
    }

    if (arr.len == 0) { return comp_.unsupported(u_, src, "an empty array at runtime"); }
    const auto slot{builder_.emit_alloca(type)};
    for (u64 i{0}; const auto value_ref : *values) {
        // A `[N]type` holds no runtime value
        if (elem.get_kind() != gs::type_kind::TYPE) {
            const auto at{
                builder_.emit_get_element_ptr(gir::value{slot, type}, {usize_constant(i)}, elem)};
            const auto given{TRY(resolve_value(value_ref, src))};
            const auto value{TRY(coerce_to(given, elem, src, {.what = use_site::kind::STORE}))};
            builder_.emit_store(gir::value{at, elem}, TRY(as_runtime(value, src))).is_initializer =
                true;
        }

        ++i;
    }

    if (arr.null_terminated) {
        const auto at{
            builder_.emit_get_element_ptr(gir::value{slot, type}, {usize_constant(arr.len)}, elem)};
        builder_
            .emit_store(gir::value{at, elem},
                        constant(comp_.constants().intern(ip::zero_value{}, elem), elem))
            .is_initializer = true;
    }

    return gir::value{builder_.emit_load(gir::value{slot, type}, type), type};
}

auto analyzer::materialize(ip::index index, const source& src) -> result<gir::value> {
    auto&       pool{comp_.constants()};
    const auto& value{pool.value_at(index)};
    auto&       type{*value.type};
    if (const auto agg{value.data.as_opt<ip::aggregate_value>()}) {
        const auto slot{builder_.emit_alloca(type)};
        for (usize i{0}; i < agg->elements.size(); ++i) {
            const auto element{agg->elements[i]};
            if (!element) { continue; }
            auto&      elem_type{[&] -> gs::type& {
                if (const auto st{type.get_data().as_opt<gs::types::struct_t>()}) {
                    return st->type_at(i);
                }

                return type.get_data().as<gs::types::array>().underlying;
            }()};
            const auto at{builder_.emit_get_element_ptr(
                gir::value{slot, type}, {usize_constant(i)}, elem_type)};
            builder_.emit_store(gir::value{at, elem_type}, TRY(as_runtime(*element, src)))
                .is_initializer = true;
        }

        if (const auto arr{type.get_data().as_opt<gs::types::array>()};
            arr && arr->null_terminated) {
            auto&      elem{arr->underlying};
            const auto at{builder_.emit_get_element_ptr(
                gir::value{slot, type}, {usize_constant(arr->len)}, elem)};
            builder_
                .emit_store(gir::value{at, elem},
                            constant(pool.intern(ip::zero_value{}, elem), elem))
                .is_initializer = true;
        }

        return gir::value{builder_.emit_load(gir::value{slot, type}, type), type};
    }

    if (const auto un{value.data.as_opt<ip::union_value>()}) {
        const auto& ut{type.get_data().as<gs::types::union_t>()};
        const auto  slot{builder_.emit_alloca(type)};
        if (ut.is_untagged) {
            if (!un->payload.empty()) {
                const auto payload{TRY(as_runtime(un->payload.front(), src))};
                builder_.emit_store(gir::value{slot, type}, payload).is_initializer = true;
            }
        } else {
            const auto field{static_cast<usize>(
                std::ranges::find(ut.field_names, un->active_field) - ut.field_names.begin())};
            auto&      i32_type{comp_.ctx().get_int(32, true)};
            const auto tag_at{builder_.emit_get_element_ptr(
                gir::value{slot, type}, {usize_constant(UNION_TAG_FIELD)}, i32_type)};
            builder_
                .emit_store(gir::value{tag_at, i32_type},
                            constant(pool.intern(static_cast<i64>(field), i32_type), i32_type))
                .is_initializer = true;
            if (!un->payload.empty()) {
                auto&      field_type{ut.type_at(field)};
                const auto payload_at{builder_.emit_get_element_ptr(
                    gir::value{slot, type}, {usize_constant(UNION_PAYLOAD_FIELD)}, field_type)};
                builder_
                    .emit_store(gir::value{payload_at, field_type},
                                TRY(as_runtime(un->payload.front(), src)))
                    .is_initializer = true;
            }
        }

        return gir::value{builder_.emit_load(gir::value{slot, type}, type), type};
    }

    return constant(index, type);
}

} // namespace ghoti::uir::sema
