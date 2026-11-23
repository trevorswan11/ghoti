#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/hash.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/arena.hh"
#include "compiler/ast/expression.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/big_int.hh"
#include "support/int128.hh"

namespace ghoti::uir::sema {

auto compilation::synthesize_enum(nominal&                                  n,
                                  gsl::span<const std::string>              names,
                                  std::vector<i64>                          values,
                                  gs::type&                                 underlying,
                                  bool                                      non_exhaustive,
                                  gsl::span<const gsl::not_null<gs::type*>> merged_from) -> void {
    if (n.state == nominal_state::RESOLVED) { return; }
    auto variant_names{make_uninit_span<std::string_view>(ctx_.arena, names.size())};
    for (usize i{0}; i < names.size(); ++i) { variant_names[i] = lasting_name(names[i]); }

    auto sources{make_uninit_span<gsl::not_null<gs::type*>>(ctx_.arena, merged_from.size())};
    std::ranges::copy(merged_from, sources.begin());
    n.enum_values = std::move(values);
    n.type->resolve<gs::types::enum_t>(gsl::span<const ast::enum_expr::enumeration>{},
                                       non_exhaustive,
                                       underlying,
                                       ctx_.pool.get_many_pending(0),
                                       *n.owner->source,
                                       gsl::span<const gsl::not_null<gs::type*>>{sources},
                                       gsl::span<const std::string_view>{variant_names});
    n.state       = nominal_state::RESOLVED;
    n.synthesized = true;
}

namespace {

// Each field's type, alignment and name, in the arena so the type outlives the analysis
struct field_spans {
    gsl::span<stdx::option<gs::type&>> types;
    gsl::span<u64>                     alignments;
    gsl::span<const std::string_view>  names;
    gsl::span<const bool>              publics;
};

} // namespace

auto compilation::synthesize_struct(nominal&                         n,
                                    gsl::span<const synthetic_field> fields,
                                    bool                             is_extern) -> void {
    if (n.state == nominal_state::RESOLVED) { return; }
    auto field_types{ctx_.pool.get_many_pending(fields.size())};
    auto alignments{ctx_.arena.make_span<u64>(fields.size())};
    auto names{make_uninit_span<std::string_view>(ctx_.arena, fields.size())};
    auto publics{make_uninit_span<bool>(ctx_.arena, fields.size())};
    for (usize i{0}; i < fields.size(); ++i) {
        field_types[i] = *fields[i].type;
        alignments[i]  = fields[i].alignment;
        names[i]       = lasting_name(fields[i].name);
        publics[i]     = fields[i].is_public;
        n.synthetic_defaults.emplace_back(fields[i].default_value);
    }

    n.type->resolve<gs::types::struct_t>(field_types,
                                         gsl::span<const ast::struct_expr::field>{},
                                         ctx_.pool.get_many_pending(0),
                                         *n.owner->source,
                                         is_extern,
                                         false,
                                         alignments,
                                         gsl::span<const std::string_view>{names},
                                         gsl::span<const bool>{publics});
    n.state       = nominal_state::RESOLVED;
    n.synthesized = true;
}

auto compilation::synthesize_union(nominal&                         n,
                                   gsl::span<const synthetic_field> fields,
                                   bool                             tagged,
                                   bool                             is_extern) -> void {
    if (n.state == nominal_state::RESOLVED) { return; }
    auto field_types{ctx_.pool.get_many_pending(fields.size())};
    auto names{make_uninit_span<std::string_view>(ctx_.arena, fields.size())};
    for (usize i{0}; i < fields.size(); ++i) {
        field_types[i] = *fields[i].type;
        names[i]       = lasting_name(fields[i].name);
    }

    n.type->resolve<gs::types::union_t>(field_types,
                                        gsl::span<const ast::union_expr::field>{},
                                        ctx_.pool.get_many_pending(0),
                                        *n.owner->source,
                                        !tagged,
                                        is_extern,
                                        false,
                                        stdx::option<gs::type&>{},
                                        gsl::span<const std::string_view>{names});
    n.state       = nominal_state::RESOLVED;
    n.synthesized = true;
}

auto merge_key_hash::operator()(const merge_key& key) const noexcept -> u64 {
    stdx::hasher h{static_cast<u64>(std::bit_cast<uptr>(key.lhs.get()))};
    h.combine(static_cast<u64>(std::bit_cast<uptr>(key.rhs.get())));
    return h.finalize();
}

auto compilation::merged_enum(const merge_key& key) -> stdx::option<gs::type&> {
    const auto found{merges_.find(key)};
    if (found == merges_.end()) { return stdx::none; }
    return *found->second;
}

auto compilation::remember_merge(const merge_key& key, gs::type& merged) -> void {
    merges_.emplace(key, &merged);
}

auto analyzer::analyze_merge_enums(inst_id inst) -> result<operand> {
    const auto&                             d{mod().data_of(inst)};
    const auto&                             src{src_of(inst)};
    auto&                                   ctx{comp_.ctx()};
    std::array<gsl::not_null<gs::type*>, 2> sides{&builtin(gs::type_kind::VOID_),
                                                  &builtin(gs::type_kind::VOID_)};
    for (usize i{0}; i < 2; ++i) {
        const auto side_ref{i == 0 ? d.bin.lhs : d.bin.rhs};
        const auto side_src{as_inst(side_ref).transform([&](inst_id s) { return src_of(s); })};
        const auto value{TRY(resolve_value(side_ref, src))};
        const auto index{comptime_value(value)};
        const bool is_type{index && comp_.constants().is_type(*index)};
        auto&      side{is_type ? comp_.constants().type_at(*index) : type_of(value)};
        if (!is_type || side.get_kind() != gs::type_kind::ENUM) {
            const bool is_bool{side.get_kind() == gs::type_kind::BOOL};
            return comp_.report(u_,
                                side_src.value_or(src),
                                gs::error::TYPE_MISMATCH,
                                fmt::format("'||' merges two enum types, but found '{}'{}",
                                            ctx.type_display_name(side),
                                            is_bool ? "; use 'or' for a logical or" : ""));
        }

        TRY(comp_.require_fields(u_, side_src.value_or(src), side));
        sides[i] = &side;
    }

    auto&           lhs{*sides[0]};
    auto&           rhs{*sides[1]};
    const merge_key key{.lhs = &lhs, .rhs = &rhs};
    if (const auto known{comp_.merged_enum(key)}) { return interned_type(*known); }

    const auto lhs_n{comp_.nominal_of(lhs)};
    const auto rhs_n{comp_.nominal_of(rhs)};
    if (!lhs_n || !rhs_n) {
        return comp_.unsupported(u_, src, "a merge of an enum UIR didn't declare");
    }

    // The left enum keeps its values, so widening from it changes no bits
    const auto&              lhs_en{lhs.get_data().as<gs::types::enum_t>()};
    const auto&              rhs_en{rhs.get_data().as<gs::types::enum_t>()};
    std::vector<std::string> names;
    std::vector<i64>         values;
    for (usize i{0}; i < lhs_en.variant_count(); ++i) {
        names.emplace_back(lhs_en.variant_names[i]);
        values.emplace_back(lhs_n->enum_values[i]);
    }

    // A tag only the right has follows the largest value so far
    i64 next{0};
    for (const auto v : values) { next = std::max(next, v + 1); }
    for (usize i{0}; i < rhs_en.variant_count(); ++i) {
        // A name both have is one tag
        const std::string name{rhs_en.variant_names[i]};
        if (std::ranges::contains(names, name)) { continue; }
        names.emplace_back(name);
        values.emplace_back(next++);
    }

    // The tag type of either side that holds every value, else the narrowest integer that does
    const auto               holds_all{[&](const gs::type& candidate) {
        return std::ranges::all_of(values, [&](i64 v) {
            return gs::comptime_int_fits(big_int{i128{v}}, candidate, comp_.ptr_bits());
        });
    }};
    gsl::not_null<gs::type*> tag_type{&lhs_en.underlying};
    if (!holds_all(*tag_type)) {
        tag_type = &rhs_en.underlying;
        if (!holds_all(*tag_type)) {
            const bool negative{std::ranges::any_of(values, [](i64 v) { return v < 0; })};
            tag_type = &ctx.get_int(64, negative);
            for (const auto bits : std::array<u16, 4>{8, 16, 32, 64}) {
                if (holds_all(ctx.get_int(bits, negative))) {
                    tag_type = &ctx.get_int(bits, negative);
                    break;
                }
            }
        }
    }

    auto&                                         n{comp_.declare_nominal(
        u_, inst, gs::type_kind::ENUM, {interned_type(lhs), interned_type(rhs)})};
    const std::array<gsl::not_null<gs::type*>, 2> sources{&lhs, &rhs};
    comp_.synthesize_enum(n,
                          names,
                          std::move(values),
                          *tag_type,
                          lhs_en.non_exhaustive || rhs_en.non_exhaustive,
                          sources);
    comp_.remember_merge(key, *n.type);
    return interned_type(*n.type);
}

auto analyzer::widen_enum(const operand& op, gs::type& target, const source& src)
    -> result<operand> {
    auto&       from{type_of(op)};
    const auto& from_en{from.get_data().as<gs::types::enum_t>()};
    const auto& to_en{target.get_data().as<gs::types::enum_t>()};

    // A known value is the variant of the same name
    if (const auto known{comptime_value(op)}) {
        const auto name{variant_name(*known)};
        if (!name) { return comp_.unsupported(u_, src, "an enum value with no variant"); }
        return enum_constant(target, *name, src);
    }

    // A one-tag enum's zero-bit value holds nothing to compare, so it is that tag
    if (from_en.variant_count() == 1) {
        return enum_constant(target, from_en.variant_names.front(), src);
    }

    const auto from_n{comp_.nominal_of(from)};
    const auto to_n{comp_.nominal_of(target)};
    if (!from_n || !to_n) {
        return comp_.unsupported(u_, src, "a widening of an enum UIR didn't declare");
    }

    // Each tag whose value moved, from its old value to its new one
    std::vector<std::pair<i64, i64>> remaps;
    for (usize i{0}; i < from_en.variant_count(); ++i) {
        for (usize j{0}; j < to_en.variant_count(); ++j) {
            if (from_en.variant_names[i] != to_en.variant_names[j]) { continue; }
            if (from_n->enum_values[i] != to_n->enum_values[j]) {
                remaps.emplace_back(from_n->enum_values[i], to_n->enum_values[j]);
            }

            break;
        }
    }

    auto&            pool{comp_.constants()};
    auto&            from_tag{from_en.underlying};
    auto&            to_tag{to_en.underlying};
    auto&            bool_type{builtin(gs::type_kind::BOOL)};
    const auto       value{TRY(as_runtime(op, src))};
    const gir::value src_int{builder_.emit_cast(gir::instruction_kind::WIDEN_CAST, value, from_tag),
                             from_tag};
    gir::value       dst_int{
        gs::is_same_unqualified(from_tag, to_tag)
            ? src_int
            : gir::value{builder_.emit_cast(gir::instruction_kind::INT_CAST, src_int, to_tag),
                         to_tag}};

    // A tag whose value moved is rewritten, and any other value keeps its bits
    if (!remaps.empty()) {
        const gir::value slot{builder_.emit_alloca(to_tag), to_tag};
        builder_.emit_store(slot, dst_int).is_initializer = true;
        for (const auto& [old_value, new_value] : remaps) {
            const auto is_tag{
                builder_.emit_binary(gir::instruction_kind::EQ,
                                     src_int,
                                     constant(pool.intern(old_value, from_tag), from_tag),
                                     bool_type)};
            const auto rewrite{new_segment()};
            const auto next{new_segment()};
            builder_.emit_cond_goto(gir::value{is_tag, bool_type}, rewrite, next);
            switch_to(rewrite);
            builder_.emit_store(slot, constant(pool.intern(new_value, to_tag), to_tag));
            emit_goto(next);
            switch_to(next);
        }

        dst_int = gir::value{builder_.emit_load(slot, to_tag), to_tag};
    }

    return operand{
        gir::value{builder_.emit_cast(gir::instruction_kind::WIDEN_CAST, dst_int, target), target}};
}

auto analyzer::known_field(ip::index object, std::string_view name, const source& src)
    -> result<stdx::option<ip::index>> {
    const auto field{TRY(comptime_field(object, name, src))};
    return comptime_value(field);
}

auto analyzer::builtin_enum(inst_id inst, gsl::span<const ref> args, const source& src)
    -> result<operand> {
    const auto desc{TRY(descriptor(args, "@Enum", "EnumInfo", src))};
    auto&      pool{comp_.constants()};
    const auto failed{[&](std::string_view what) -> result<operand> {
        return comp_.report(
            u_, src, gs::error::COMPTIME_EVALUATION_FAILED, fmt::format("'@Enum': {}", what));
    }};

    const auto tag_index{TRY(known_field(desc, "tag_type", src))};
    const auto fields_index{TRY(known_field(desc, "fields", src))};
    const auto exhaustive_index{TRY(known_field(desc, "exhaustive", src))};
    if (!tag_index || !pool.is_type(*tag_index)) {
        return failed("descriptor is missing 'tag_type'");
    }
    const auto fields{fields_index ? pool.value_at(*fields_index).data.as_opt<ip::aggregate_value>()
                                   : stdx::none};
    if (!fields) { return failed("descriptor is missing 'fields'"); }
    const auto exhaustive{exhaustive_index ? pool.value_at(*exhaustive_index).data.as_opt<bool>()
                                           : stdx::none};
    if (!exhaustive) { return failed("descriptor is missing 'exhaustive'"); }

    // Reading the fields can intern, which may move what the pool holds
    const bool non_exhaustive{!*exhaustive};

    std::vector<std::string>                   names;
    std::vector<i64>                           values;
    const std::vector<stdx::option<ip::index>> elements{fields->elements};
    for (const auto element : elements) {
        if (!element) {
            return failed("every element of 'fields' needs a compile-time 'name' and 'value'");
        }

        const auto name_index{TRY(known_field(*element, "name", src))};
        const auto value_index{TRY(known_field(*element, "value", src))};
        const auto name{name_index ? pool.value_at(*name_index).data.as_opt<std::string>()
                                   : stdx::none};
        const auto value{value_index ? int_of(pool.value_at(*value_index)) : stdx::none};
        if (!name || !value) {
            return failed("every element of 'fields' needs a compile-time 'name' and 'value'");
        }

        const auto small{value->to_i128()};
        if (!small || *small > std::numeric_limits<i64>::max() ||
            *small < std::numeric_limits<i64>::min()) {
            return comp_.unsupported(u_, src, "an enum value past 64 bits");
        }

        names.emplace_back(*name);
        values.emplace_back(static_cast<i64>(*small));
    }

    auto& tag_type{pool.type_at(*tag_index)};
    auto& n{comp_.declare_nominal(u_, inst, gs::type_kind::ENUM, {desc})};
    comp_.synthesize_enum(n, names, std::move(values), tag_type, non_exhaustive, {});
    return interned_type(*n.type);
}

auto analyzer::descriptor_fields(ip::index        desc,
                                 std::string_view builtin_name,
                                 bool             is_struct,
                                 const source&    src) -> result<std::vector<synthetic_field>> {
    auto&      pool{comp_.constants()};
    const auto failed{[&](std::string_view what) -> result<std::vector<synthetic_field>> {
        return comp_.report(u_,
                            src,
                            gs::error::COMPTIME_EVALUATION_FAILED,
                            fmt::format("'{}': {}", builtin_name, what));
    }};

    const auto fields_index{TRY(known_field(desc, "fields", src))};
    const auto fields{fields_index ? pool.value_at(*fields_index).data.as_opt<ip::aggregate_value>()
                                   : stdx::none};
    if (!fields) { return failed("descriptor is missing 'fields'"); }

    const std::vector<stdx::option<ip::index>> elements{fields->elements};
    const auto missing{is_struct
                           ? "every element of 'fields' needs a compile-time 'name', 'type'"
                           : "every element of 'fields' needs a compile-time 'name' and 'type'"};
    std::vector<synthetic_field> out;
    for (const auto element : elements) {
        if (!element) { return failed(missing); }
        const auto name_index{TRY(known_field(*element, "name", src))};
        const auto type_index{TRY(known_field(*element, "type", src))};
        const auto name{name_index ? pool.value_at(*name_index).data.as_opt<std::string>()
                                   : stdx::none};
        if (!name || !type_index || !pool.is_type(*type_index)) { return failed(missing); }
        synthetic_field field{.name = *name, .type = &pool.type_at(*type_index)};
        if (!is_struct) {
            out.emplace_back(std::move(field));
            continue;
        }

        // A default is a pointer to the value, known at compile time
        const auto default_index{TRY(known_field(*element, "default_value", src))};
        if (default_index) {
            const auto& default_data{pool.value_at(*default_index).data};
            if (const auto pointed{default_data.as_opt<ip::address_value>()};
                pointed && pointed->pointee.size() == 1) {
                field.default_value = pointed->pointee.front();
            } else if (!default_data.is<ip::nullptr_value>()) {
                return comp_.unsupported(u_, src, "a field default that isn't a known value");
            }
        }

        const auto align_index{TRY(known_field(*element, "alignment", src))};
        const auto requested{align_index ? int_of(pool.value_at(*align_index)) : stdx::none};
        const auto alignment{requested ? requested->to_u128().value_or(0) : u128{0}};
        if (alignment != 0 && (alignment & (alignment - 1)) != 0) {
            return failed(fmt::format("field '{}' alignment {} is not a power of two",
                                      field.name,
                                      requested->to_string()));
        }

        const auto pub_index{TRY(known_field(*element, "is_pub", src))};
        const auto is_pub{pub_index ? pool.value_at(*pub_index).data.as_opt<bool>() : stdx::none};
        field.alignment = static_cast<u64>(alignment);
        field.is_public = is_pub && *is_pub;
        out.emplace_back(std::move(field));
    }

    return out;
}

auto analyzer::builtin_struct(inst_id inst, gsl::span<const ref> args, const source& src)
    -> result<operand> {
    const auto desc{TRY(descriptor(args, "@Struct", "StructInfo", src))};
    auto&      pool{comp_.constants()};
    const auto is_extern_index{TRY(known_field(desc, "is_extern", src))};
    const auto is_packed_index{TRY(known_field(desc, "is_packed", src))};
    const auto is_extern{is_extern_index ? pool.value_at(*is_extern_index).data.as_opt<bool>()
                                         : stdx::none};
    const auto is_packed{is_packed_index ? pool.value_at(*is_packed_index).data.as_opt<bool>()
                                         : stdx::none};
    if (!is_extern || !is_packed) {
        return comp_.report(u_,
                            src,
                            gs::error::COMPTIME_EVALUATION_FAILED,
                            fmt::format("'@Struct': descriptor is missing '{}'",
                                        is_extern ? "is_packed" : "is_extern"));
    }

    const bool extern_struct{*is_extern};
    if (*is_packed) { return comp_.unsupported(u_, src, "a packed struct"); }
    const auto fields{TRY(descriptor_fields(desc, "@Struct", true, src))};
    auto&      n{comp_.declare_nominal(u_, inst, gs::type_kind::STRUCT, {desc})};
    comp_.synthesize_struct(n, fields, extern_struct);
    return interned_type(*n.type);
}

auto analyzer::builtin_union(inst_id inst, gsl::span<const ref> args, const source& src)
    -> result<operand> {
    const auto desc{TRY(descriptor(args, "@Union", "UnionInfo", src))};
    auto&      pool{comp_.constants()};
    const auto read_bool{[&](std::string_view name) -> result<stdx::option<bool>> {
        const auto index{TRY(known_field(desc, name, src))};
        const auto value{index ? pool.value_at(*index).data.as_opt<bool>() : stdx::none};
        return value ? stdx::option<bool>{*value} : stdx::option<bool>{};
    }};
    const auto is_extern{TRY(read_bool("is_extern"))};
    const auto is_packed{TRY(read_bool("is_packed"))};
    const auto tagged{TRY(read_bool("tagged"))};
    for (const auto& [value, name] : {std::pair{is_extern, "is_extern"},
                                      std::pair{is_packed, "is_packed"},
                                      std::pair{tagged, "tagged"}}) {
        if (!value) {
            return comp_.report(u_,
                                src,
                                gs::error::COMPTIME_EVALUATION_FAILED,
                                fmt::format("'@Union': descriptor is missing '{}'", name));
        }
    }

    if (*is_packed) { return comp_.unsupported(u_, src, "a packed union"); }
    const auto tag_index{TRY(known_field(desc, "tag_type", src))};
    if (tag_index && pool.is_type(*tag_index) &&
        pool.type_at(*tag_index).get_kind() != gs::type_kind::VOID_) {
        return comp_.unsupported(u_, src, "a union tagged by its own enum");
    }

    const auto fields{TRY(descriptor_fields(desc, "@Union", false, src))};
    auto&      n{comp_.declare_nominal(u_, inst, gs::type_kind::UNION, {desc})};
    comp_.synthesize_union(n, fields, *tagged, *is_extern);
    return interned_type(*n.type);
}

} // namespace ghoti::uir::sema
