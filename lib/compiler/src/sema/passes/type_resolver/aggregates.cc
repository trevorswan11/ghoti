#include <algorithm>
#include <string_view>
#include <utility>

#include <gsl/pointers>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/arena.hh"
#include "compiler/ast/attributes.hh"
#include "compiler/ast/format.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/traits.hh"
#include "compiler/ast/visitor.hh"
#include "compiler/gir/const_eval.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/passes/type_resolver.hh"
#include "compiler/sema/side_tables.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "internal.hh"
#include "support/diagnostic.hh"
#include "support/int128.hh"

namespace ghoti::sema {

using namespace resolver_detail;

auto type_resolver::resolve_members(gsl::span<stdx::option<type&>>      buf,
                                    gsl::span<const ast::member_handle> members)
    -> stdx::option<gsl::span<stdx::option<type&>>> {
    // Only poison the enum once all members are collected
    for (usize i{0}; const auto& member : members) {
        // The resolved statement type is in last_type_ unlike in the symbol collector
        resolve(*member);
        auto& member_type{resolving_.get_sema_type(member)};
        if (member_type.is_poison()) { return stdx::none; }
        buf[i++] = member_type;
    }
    return buf;
}

auto type_resolver::check_enum_value(enum_value_tracker&            values,
                                     ast::identifier_handle         name,
                                     stdx::option<ast::expr_handle> value)
    -> stdx::option<diagnostic> {
    if (!is_integer(values.underlying.get_kind())) { return stdx::none; }

    stdx::option<i128> current;
    if (value) {
        gir::const_eval evaluator{ctx_, resolving_};
        if (const auto folded{evaluator.try_eval(*value)}) { current = folded->as_int_opt(); }
    } else {
        current = values.next;
    }
    // A value that does not fold here is left for the emitter to report
    values.next = current ? stdx::option<i128>{*current + 1} : stdx::none;
    if (!current) { return stdx::none; }

    const auto& member{resolving_.ast.get_as<ast::identifier_expr>(name).name};
    const auto  loc{resolving_.ast.location_of(value ? ast::node_id{*value} : ast::node_id{name})};
    if (!comptime_int_fits(*current, values.underlying, target_ptr_bits())) {
        return diagnostic{fmt::format("Enum member '{}' has a value that does not fit its "
                                      "underlying type '{}'",
                                      member,
                                      ctx_.type_display_name(values.underlying)),
                          error::TYPE_MISMATCH,
                          loc};
    }
    const auto duplicate{
        std::ranges::find(values.seen, *current, &std::pair<i128, std::string_view>::first)};
    if (duplicate != values.seen.end()) {
        return diagnostic{fmt::format("Enum members '{}' and '{}' share the same value",
                                      duplicate->second,
                                      member),
                          error::TYPE_MISMATCH,
                          loc};
    }
    values.seen.emplace_back(*current, member);
    return stdx::none;
}

// Aggregate literals are only typed where the symbol collector can name them (a declaration's
// value, a field type, a returned type), never mid-expression like `struct {...}.len`
template <ast::IndexableID ID>
auto type_resolver::untyped_aggregate_literal(ID id, std::string_view kind) -> type& {
    return ctx_.poison_node(
        resolving_,
        id,
        fmt::format("An anonymous {0} type cannot be used in an expression; declare it first "
                    "(e.g. `const T = {0} {{ ... }};`)",
                    kind),
        error::TYPE_MISMATCH,
        resolving_.ast.location_of(id));
}

template <ast::IndexableID ID>
auto type_resolver::visit(ID id, const ast::enum_expr& enum_expr) -> void {
    PROFILE_FUNCTION();
    if (enum_expr.underlying) { resolve(*enum_expr.underlying); }

    const auto enum_type_opt{resolving_.get_sema_type_opt(id)};
    if (!enum_type_opt) { return last_type_.emplace(untyped_aggregate_literal(id, "enum")); }
    auto& enum_type{*enum_type_opt};
    if (enum_type.is_poison() || !enum_type.has_symbol_table_idx()) {
        return last_type_.emplace(enum_type.is_poison() ? enum_type
                                                        : ctx_.poison_node(resolving_, id));
    }
    // See the note in `visit(struct_expr)`: a recursive re-entry is a no-op.
    if (std::ranges::contains(resolving_aggregate_nodes_, id.get_index())) {
        return last_type_.emplace(enum_type);
    }
    const aggregate_resolve_guard agg_guard{resolving_aggregate_nodes_, id.get_index()};
    const scope                   s{table_stack_, enum_type.get_symbol_table_idx(), table_idx_};
    const structural_guard        g{user_type_stack_, enum_type};

    // The underlying type defaults to an i32 as it would in C or C++
    auto& underlying_type{enum_expr.underlying
                              ? denoted_type(resolving_.get_sema_type(*enum_expr.underlying))
                              : ctx_.get_int(32, true)};

    enum_value_tracker values{underlying_type};
    for (const auto& [name, value] : enum_expr.enumerations) {
        if (value) {
            const structural_guard value_g{implicit_type_stack_, underlying_type};
            TRY_RESOLVE(*value);
        }
        if (auto diag{check_enum_value(values, name, value)}) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(*diag)));
        }

        // The value's type doesn't contribute to the actual enumeration type
        const auto& ident{resolving_.ast.get_as<ast::identifier_expr>(name)};
        auto        sym{ctx_.registry.get_from_opt(table_idx_, ident.name)};
        if (!sym) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }
        resolving_.set_sema_type(name, underlying_type);
        sym->set_kind(symbol_kind::VALUE);
        sym->set_status(symbol_status::RESOLVED);
    }

    auto member_types{ctx_.pool.get_many_pending(enum_expr.members.size())};
    committable_resolution<types::enum_t> resolution{enum_type,
                                                     enum_expr.enumerations,
                                                     enum_expr.non_exhaustive,
                                                     underlying_type,
                                                     member_types,
                                                     resolving_};
    lend_shape_to_pending_instance(id.get_index(), enum_type);
    if (!resolve_members(member_types, enum_expr.members)) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id));
    }

    resolution.commit();
    last_type_.emplace(enum_type);
}

VISITOR_TEMPLATE_INIT(type_resolver, visit, const ast::enum_expr&)

namespace {

[[nodiscard]] auto incomplete_field(std::string_view name, const source_location& location)
    -> diagnostic {
    return diagnostic{
        fmt::format("Field '{}' has an incomplete type; creates an infinite size cycle", name),
        error::CYCLIC_DEPENDENCY,
        location};
}

[[nodiscard]] auto illegal_auto_field(std::string_view       kind,
                                      std::string_view       name,
                                      const source_location& location) -> diagnostic {
    return diagnostic{fmt::format("{} field '{}' cannot have type 'auto'", kind, name),
                      error::ILLEGAL_AUTO_USAGE,
                      location};
}

[[nodiscard]] auto extern_reference_field(std::string_view       kind,
                                          std::string_view       name,
                                          const source_location& location) -> diagnostic {
    return diagnostic{
        fmt::format("extern {} field '{}' cannot have a reference type; an 'extern' aggregate "
                    "has no ABI representation for references, use a raw pointer ('^T') instead",
                    kind,
                    name),
        error::ILLEGAL_REFERENCE_FIELD,
        location};
}

[[nodiscard]] auto cabi_dyn_field(std::string_view       kind,
                                  std::string_view       name,
                                  const source_location& location) -> diagnostic {
    return diagnostic{
        fmt::format("extern {} field '{}' cannot involve `dyn` in any form; a `&dyn` / `^dyn` fat "
                    "pointer has no C ABI representation",
                    kind,
                    name),
        error::ILLEGAL_REFERENCE_FIELD,
        location};
}

[[nodiscard]] auto cabi_erased_fn_field(std::string_view       kind,
                                        std::string_view       name,
                                        const source_location& location) -> diagnostic {
    return diagnostic{
        fmt::format("extern {} field '{}' holds an erased `fn(...)` callable, which has no C ABI "
                    "representation; use a thin `extern fn(...)` instead",
                    kind,
                    name),
        error::ILLEGAL_REFERENCE_FIELD,
        location};
}

[[nodiscard]] auto cabi_nonabi_scalar_field(std::string_view       kind,
                                            std::string_view       name,
                                            std::string_view       type_name,
                                            const source_location& location) -> diagnostic {
    return diagnostic{
        fmt::format("extern {} field '{}' has type '{}', which has no C ABI representation; "
                    "extern signatures accept 8/16/32/64-bit integers, usize/isize, bool, f32, "
                    "f64, and f80 on x86",
                    kind,
                    name,
                    type_name),
        error::ILLEGAL_REFERENCE_FIELD,
        location};
}

} // namespace

template <ast::IndexableID ID>
auto type_resolver::visit(ID id, const ast::struct_expr& struct_expr) -> void {
    PROFILE_FUNCTION();
    const auto struct_type_opt{resolving_.get_sema_type_opt(id)};
    if (!struct_type_opt) { return last_type_.emplace(untyped_aggregate_literal(id, "struct")); }
    auto& struct_type{*struct_type_opt};
    if (struct_type.is_poison() || !struct_type.has_symbol_table_idx()) {
        return last_type_.emplace(struct_type.is_poison() ? struct_type
                                                          : ctx_.poison_node(resolving_, id));
    }
    // Recursive re-entry: a member type names this struct while it is already being resolved
    // further up the stack. Don't kick off a nested resolution
    if (std::ranges::contains(resolving_aggregate_nodes_, id.get_index())) {
        return last_type_.emplace(struct_type);
    }
    const aggregate_resolve_guard agg_guard{resolving_aggregate_nodes_, id.get_index()};
    const scope                   s{table_stack_, struct_type.get_symbol_table_idx(), table_idx_};
    const structural_guard        g{user_type_stack_, struct_type};

    auto field_types{ctx_.pool.get_many_pending(struct_expr.fields.size())};
    for (usize i{0}; const auto& field : struct_expr.fields) {
        const auto& ident{resolving_.ast.get_as<ast::identifier_expr>(field.name)};
        auto        sym{ctx_.registry.get_from_opt(table_idx_, ident.name)};
        if (!sym) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }

        sym->set_status(symbol_status::RESOLVING);
        TRY_RESOLVE(field.explicit_type);
        resolving_.set_identifier_declaration(field.name,
                                              declaration_ref{.owner      = &resolving_,
                                                              .decl       = stdx::none,
                                                              .annotation = field.explicit_type});
        // `f: @TypeOf(g)` / `f: FnAlias` stores the denoted type, not a `type` value
        gsl::not_null<type*> field_type{&denoted_type(*last_type_.take())};
        if (reject_storage_slot(field.explicit_type, *field_type) ||
            reject_poly_storage(field.explicit_type)) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id));
        }

        if (field_type->get_kind() == type_kind::AUTO) {
            if (!field.default_value) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    illegal_auto_field(
                        "Struct", ident.name, resolving_.ast.location_of(field.explicit_type))));
            }
            TRY_RESOLVE(*field.default_value);
            field_type = last_type_.take();
            if (field_type->get_kind() == type_kind::AUTO) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    illegal_auto_field(
                        "Struct", ident.name, resolving_.ast.location_of(field.explicit_type))));
            }
            // A field type inferred from an unsuffixed literal default materializes.
            field_type = &comptime_numeric_view(*field_type);
        } else if (field.default_value) {
            const structural_guard inner_g{implicit_type_stack_, *field_type};
            TRY_RESOLVE(*field.default_value);
        }

        if (!field_type->is_resolved()) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                incomplete_field(ident.name, resolving_.ast.location_of(field.explicit_type))));
        }

        // A bit-packed `packed struct` may only hold packed-eligible fields
        if (struct_expr.is_packed && !struct_expr.is_extern &&
            !sema::packed_field_bits(*field_type, target_ptr_bits())) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("field '{}' of type '{}' cannot appear in a 'packed struct'; only "
                            "integers, floats, 'bool', enums, pointers, packed aggregates, and "
                            "fixed arrays of those are allowed",
                            ident.name,
                            ctx_.type_display_name(*field_type)),
                error::ILLEGAL_PACKED_FIELD,
                resolving_.ast.location_of(field.explicit_type)));
        }
        if (struct_expr.is_packed && !struct_expr.is_extern && field.explicit_alignment()) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("a 'packed struct' field cannot specify 'align' (field '{}')",
                            ident.name),
                error::ILLEGAL_PACKED_FIELD,
                resolving_.ast.location_of(field.explicit_type)));
        }

        if (struct_expr.is_extern) {
            const auto bad{scan_cabi_offenders(*field_type, target_has_x86_fp80())};
            const auto loc{resolving_.ast.location_of(field.explicit_type)};
            if (bad.has_dyn) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_, id, cabi_dyn_field("struct", ident.name, loc)));
            }
            if (bad.has_erased_fn) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_, id, cabi_erased_fn_field("struct", ident.name, loc)));
            }
            if (bad.has_ref) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_, id, extern_reference_field("struct", ident.name, loc)));
            }
            if (bad.has_nonabi_scalar) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    cabi_nonabi_scalar_field(
                        "struct", ident.name, ctx_.type_display_name(*field_type), loc)));
            }
        }

        resolving_.set_sema_type(field.name, *field_type);
        sym->set_kind(symbol_kind::VALUE);
        sym->set_status(symbol_status::RESOLVED);
        field_types[i++] = *field_type;
    }

    auto member_types{ctx_.pool.get_many_pending(struct_expr.members.size())};
    auto field_alignments{ctx_.arena.make_span<u64>(struct_expr.fields.size())};
    for (usize i{0}; const auto& field : struct_expr.fields) {
        field_alignments[i++] = resolve_field_attributes(field.attributes).value_or(0);
    }

    if (struct_expr.is_packed && !struct_expr.is_extern) {
        u64 total_bits{0};
        for (const auto ft : field_types) {
            total_bits += ft ? sema::packed_field_bits(*ft, target_ptr_bits()).value_or(0) : 0;
        }
        if (total_bits < 1 || total_bits > 128) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("a 'packed struct' backing width of {} bits is unsupported (the "
                            "current limit is 128 bits)",
                            total_bits),
                error::ILLEGAL_PACKED_FIELD,
                resolving_.ast.location_of(id)));
        }
    }

    committable_resolution<types::struct_t> resolution{struct_type,
                                                       field_types,
                                                       struct_expr.fields,
                                                       member_types,
                                                       resolving_,
                                                       struct_expr.is_extern,
                                                       struct_expr.is_packed,
                                                       field_alignments};
    lend_shape_to_pending_instance(id.get_index(), struct_type);
    if (!resolve_members(member_types, struct_expr.members)) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id));
    }

    resolution.commit();
    last_type_.emplace(struct_type);
}

VISITOR_TEMPLATE_INIT(type_resolver, visit, const ast::struct_expr&)

// A `union(MyEnum)` has one field per variant of `MyEnum`, each named for its variant. A
// non-exhaustive enum's unnamed values have no field, so the union can never hold them
template <ast::IndexableID ID>
auto type_resolver::check_union_tag_enum(const ast::union_expr& union_expr, type& tag_enum, ID id)
    -> stdx::option<diagnostic> {
    const auto& en{tag_enum.get_data().as<types::enum_t>()};
    const auto  variant_name{[&](const ast::enum_expr::enumeration& e) -> std::string_view {
        return en.enclosing.ast.get_as<ast::identifier_expr>(e.name).name;
    }};
    for (const auto& field : union_expr.fields) {
        const auto& name{resolving_.ast.get_as<ast::identifier_expr>(field.name).name};
        if (!std::ranges::any_of(en.ast_enumerations,
                                 [&](const auto& e) { return variant_name(e) == name; })) {
            return diagnostic{fmt::format("Union field '{}' is not a variant of its tag enum '{}'",
                                          name,
                                          ctx_.type_display_name(tag_enum)),
                              error::ILLEGAL_UNION_TAG,
                              resolving_.ast.location_of(field.name)};
        }
    }
    for (const auto& e : en.ast_enumerations) {
        const auto name{variant_name(e)};
        if (!std::ranges::any_of(union_expr.fields, [&](const auto& field) {
                return resolving_.ast.get_as<ast::identifier_expr>(field.name).name == name;
            })) {
            return diagnostic{fmt::format("Variant '{}' of the tag enum '{}' has no union field",
                                          name,
                                          ctx_.type_display_name(tag_enum)),
                              error::ILLEGAL_UNION_TAG,
                              resolving_.ast.location_of(id)};
        }
    }
    return stdx::none;
}

template <ast::IndexableID ID>
auto type_resolver::visit(ID id, const ast::union_expr& union_expr) -> void {
    PROFILE_FUNCTION();
    const auto union_type_opt{resolving_.get_sema_type_opt(id)};
    if (!union_type_opt) { return last_type_.emplace(untyped_aggregate_literal(id, "union")); }
    auto& union_type{*union_type_opt};
    if (union_type.is_poison() || !union_type.has_symbol_table_idx()) {
        return last_type_.emplace(union_type.is_poison() ? union_type
                                                         : ctx_.poison_node(resolving_, id));
    }
    // A recursive re-entry is a no-op.
    if (std::ranges::contains(resolving_aggregate_nodes_, id.get_index())) {
        return last_type_.emplace(union_type);
    }
    const aggregate_resolve_guard agg_guard{resolving_aggregate_nodes_, id.get_index()};

    // `union(MyEnum)` / `union(void)` name their tag from where the union is written
    stdx::option<type&> tag_enum;
    bool                void_tag{false};
    if (union_expr.tag_type) {
        TRY_RESOLVE(*union_expr.tag_type);
        auto& tag{denoted_type(*last_type_.take())};
        if (tag.get_kind() == type_kind::VOID_) {
            void_tag = true;
        } else if (tag.get_kind() == type_kind::ENUM && tag.is_resolved()) {
            tag_enum = tag;
        } else {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("A union's tag must be an enum, or `void` for no tag; found '{}'",
                            ctx_.type_display_name(tag)),
                error::ILLEGAL_UNION_TAG,
                resolving_.ast.location_of(*union_expr.tag_type)));
        }
    }

    const scope            s{table_stack_, union_type.get_symbol_table_idx(), table_idx_};
    const structural_guard g{user_type_stack_, union_type};

    auto field_types{ctx_.pool.get_many_pending(union_expr.fields.size())};
    for (usize i{0}; const auto& field : union_expr.fields) {
        const auto& ident{resolving_.ast.get_as<ast::identifier_expr>(field.name)};
        auto        sym{ctx_.registry.get_from_opt(table_idx_, ident.name)};
        if (!sym) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }

        sym->set_status(symbol_status::RESOLVING);
        TRY_RESOLVE(field.explicit_type);
        resolving_.set_identifier_declaration(field.name,
                                              declaration_ref{.owner      = &resolving_,
                                                              .decl       = stdx::none,
                                                              .annotation = field.explicit_type});
        auto& field_type{denoted_type(*last_type_.take())};
        if (reject_storage_slot(field.explicit_type, field_type) ||
            reject_poly_storage(field.explicit_type)) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id));
        }

        if (field_type.get_kind() == type_kind::AUTO) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                illegal_auto_field(
                    "Union", ident.name, resolving_.ast.location_of(field.explicit_type))));
        }

        if (!field_type.is_resolved()) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                incomplete_field(ident.name, resolving_.ast.location_of(field.explicit_type))));
        }

        if (union_expr.is_extern) {
            const auto bad{scan_cabi_offenders(field_type, target_has_x86_fp80())};
            const auto loc{resolving_.ast.location_of(field.explicit_type)};
            if (bad.has_dyn) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_, id, cabi_dyn_field("union", ident.name, loc)));
            }
            if (bad.has_erased_fn) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_, id, cabi_erased_fn_field("union", ident.name, loc)));
            }
            if (bad.has_ref) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_, id, extern_reference_field("union", ident.name, loc)));
            }
            if (bad.has_nonabi_scalar) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    cabi_nonabi_scalar_field(
                        "union", ident.name, ctx_.type_display_name(field_type), loc)));
            }
        }

        // A bit-packed `packed union` may only hold packed-eligible fields
        if (union_expr.is_packed && !union_expr.is_extern &&
            !sema::packed_field_bits(field_type, target_ptr_bits())) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("field '{}' of type '{}' cannot appear in a 'packed union'; only "
                            "integers, floats, 'bool', enums, pointers, packed aggregates, and "
                            "fixed arrays of those are allowed",
                            ident.name,
                            ctx_.type_display_name(field_type)),
                error::ILLEGAL_PACKED_FIELD,
                resolving_.ast.location_of(field.explicit_type)));
        }
        if (resolve_field_attributes(field.attributes)) {
            ctx_.diags.emplace_back("Attribute 'align' is not supported on union fields",
                                    error::ILLEGAL_ATTRIBUTE,
                                    resolving_.ast.location_of(field.name));
        }
        if (union_expr.is_packed && !union_expr.is_extern && field.explicit_alignment()) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("a 'packed union' field cannot specify 'align' (field '{}')",
                            ident.name),
                error::ILLEGAL_PACKED_FIELD,
                resolving_.ast.location_of(field.explicit_type)));
        }

        resolving_.set_sema_type(field.name, field_type);
        sym->set_kind(symbol_kind::VALUE);
        sym->set_status(symbol_status::RESOLVED);
        field_types[i++] = field_type;
    }

    if (union_expr.is_packed && !union_expr.is_extern) {
        u64        widest{0};
        const auto ptr_bits{target_ptr_bits()};
        for (const auto ft : field_types) {
            widest =
                std::max<u64>(widest, ft ? sema::packed_field_bits(*ft, ptr_bits).value_or(0) : 0);
        }
        if (widest < 1 || widest > 128) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("a 'packed union' backing width of {} bits is unsupported (the "
                            "current limit is 128 bits)",
                            widest),
                error::ILLEGAL_PACKED_FIELD,
                resolving_.ast.location_of(id)));
        }
    }

    if (tag_enum) {
        if (auto diag{check_union_tag_enum(union_expr, *tag_enum, id)}) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(*diag)));
        }
    }

    auto member_types{ctx_.pool.get_many_pending(union_expr.members.size())};
    committable_resolution<types::union_t> resolution{union_type,
                                                      field_types,
                                                      union_expr.fields,
                                                      member_types,
                                                      resolving_,
                                                      union_expr.is_extern ||
                                                          union_expr.is_packed || void_tag,
                                                      union_expr.is_extern,
                                                      union_expr.is_packed,
                                                      tag_enum};
    lend_shape_to_pending_instance(id.get_index(), union_type);
    if (!resolve_members(member_types, union_expr.members)) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id));
    }

    resolution.commit();
    last_type_.emplace(union_type);
}

VISITOR_TEMPLATE_INIT(type_resolver, visit, const ast::union_expr&)

// Builds the `types::function` for a bodyless interface requirement
auto type_resolver::resolve_required_method_type(const ast::function_expr& fn,
                                                 type& self_placeholder) -> type& {
    const auto param_count{fn.parameters.size() + (fn.self ? 1UZ : 0UZ)};
    auto       params{ctx_.pool.get_many_unsafe(param_count)};
    usize      idx{0};

    if (fn.self) {
        gsl::not_null<type*> self_ty{&self_placeholder};
        if (const auto mut{types::mut::from_type_modifier(fn.self->modifier)}) {
            self_ty = fn.self->modifier.is_ptr() ? &ctx_.get_pointer(*mut, self_placeholder)
                                                 : &ctx_.get_reference(*mut, self_placeholder);
        }
        params[idx++] = self_ty;
    }
    for (const auto& param : fn.parameters) {
        resolve_param_type(param.explicit_type);
        params[idx++] = &denoted_type(*last_type_.take());
    }
    resolve(fn.explicit_return_type);
    auto& return_type{denoted_type(*last_type_.take())};

    types::key_t key{type_kind::FUNCTION, types::mut::CONSTANT};
    for (const auto p : params) { key.imprint(*p); }
    key.imprint(return_type);
    key.imprint(fn.conv);
    auto& fn_type{*ctx_.pool[key]};
    fn_type.resolve_if<types::function>(
        params, return_type, fn.self.has_value(), fn.variadic, fn.conv);
    return fn_type;
}

template <ast::IndexableID ID>
auto type_resolver::visit(ID id, const ast::interface_expr& iface) -> void {
    PROFILE_FUNCTION();
    const auto iface_type_opt{resolving_.get_sema_type_opt(id)};
    if (!iface_type_opt) { return last_type_.emplace(untyped_aggregate_literal(id, "interface")); }
    auto& iface_type{*iface_type_opt};

    const auto method_count{iface.methods.size()};
    auto       method_sigs{make_uninit_span<gsl::not_null<type*>>(ctx_.arena, method_count)};
    auto       method_src{ctx_.arena.make_span<usize>(method_count)};
    auto       method_names{ctx_.arena.make_span<std::string_view>(method_count)};
    auto       method_is_pub{ctx_.arena.make_span<bool>(method_count)};

    {
        const scope            s{table_stack_, iface_type.get_symbol_table_idx(), table_idx_};
        const structural_guard g{user_type_stack_, iface_type};

        for (const auto& at : iface.assoc_types) {
            TRY_RESOLVE(at.annotation);
            auto& annotation{*last_type_.take()};
            if (annotation.get_kind() != type_kind::TYPE && !annotation.is_poison()) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    fmt::format("associated type `{}` must be annotated `: type`",
                                resolving_.ast.get_as<ast::identifier_expr>(*at.name).name),
                    error::TYPE_MISMATCH,
                    resolving_.ast.location_of(at.annotation)));
            }
            auto& placeholder{annotation.is_poison()
                                  ? annotation
                                  : assoc_type_placeholder((*at.name).get_index())};
            resolving_.set_sema_type(at.name, placeholder);
            resolve_symbol_info(at.name, symbol_kind::TYPE);
            if (at.default_type) { TRY_RESOLVE(*at.default_type); }
        }

        for (const auto& ac : iface.assoc_consts) {
            TRY_RESOLVE(ac.explicit_type);
            auto& const_type{denoted_type(*last_type_.take())};
            resolving_.set_sema_type(ac.name, const_type);
            resolve_symbol_info(ac.name, symbol_kind::VALUE);
        }

        // Build the method signatures only (params / return / self) requirements-first, so
        // `requirement_count` partitions the arrays.
        usize      out{0};
        const auto emit_pass{[&](bool want_required) -> void {
            for (usize src{0}; src < iface.methods.size(); ++src) {
                const auto& m{iface.methods[src]};
                const auto& fn{resolving_.ast.get_as<ast::function_expr>(*m.signature)};
                if (fn.is_type_expr != want_required) { continue; }

                method_sigs[out] = &resolve_required_method_type(fn, iface_type);
                if (m.attributes) {
                    attribute_refs items;
                    for (const auto& item : m.attributes->items) { items.emplace_back(&item); }
                    const auto fn_data{method_sigs[out]->get_data().as_opt<types::function>()};
                    const bool returns_void{fn_data &&
                                            fn_data->return_type.get_kind() == type_kind::VOID_};
                    resolving_.set_node_attributes(
                        *m.signature,
                        resolve_attributes(items, ast::attribute_target::FN_DECL, returns_void));
                }
                method_src[out]    = src;
                method_names[out]  = resolving_.ast.get_as<ast::identifier_expr>(*m.name).name;
                method_is_pub[out] = m.is_public();
                ++out;
            }
        }};
        emit_pass(true);

        // Default-method *bodies* are resolved after the interface type is committed, so
        // `self.method(...)` inside them can see the set.
        const auto requirement_count{out};
        emit_pass(false);

        auto assoc_type_names{ctx_.arena.make_span<std::string_view>(iface.assoc_types.size())};
        for (usize i{0}; const auto& at : iface.assoc_types) {
            assoc_type_names[i++] = resolving_.ast.get_as<ast::identifier_expr>(*at.name).name;
        }
        auto assoc_const_names{ctx_.arena.make_span<std::string_view>(iface.assoc_consts.size())};
        for (usize i{0}; const auto& ac : iface.assoc_consts) {
            assoc_const_names[i++] = resolving_.ast.get_as<ast::identifier_expr>(*ac.name).name;
        }

        committable_resolution<types::interface_t> resolution{iface_type,
                                                              method_sigs,
                                                              method_src,
                                                              method_names,
                                                              method_is_pub,
                                                              requirement_count,
                                                              assoc_type_names,
                                                              assoc_const_names,
                                                              iface.methods,
                                                              iface.assoc_types,
                                                              iface.assoc_consts,
                                                              resolving_};
        resolution.commit();

        // With the interface type committed, default-method bodies and defaulted associated
        // items can now resolve `self.method(...)` / `Self`-typed references against the set.
        for (const auto& at : iface.assoc_types) {
            if (at.default_type) { TRY_RESOLVE(*at.default_type); }
        }
        for (const auto& ac : iface.assoc_consts) {
            if (ac.default_value) {
                const structural_guard inner{implicit_type_stack_,
                                             resolving_.get_sema_type(ac.name)};
                TRY_RESOLVE(*ac.default_value);
            }
        }
        for (const auto& m : iface.methods) {
            const auto& fn{resolving_.ast.get_as<ast::function_expr>(*m.signature)};
            if (!fn.is_type_expr) {
                resolve(m.signature);
                DISCARD(last_type_.take());
            }
        }
    }

    last_type_.emplace(iface_type);
}

VISITOR_TEMPLATE_INIT(type_resolver, visit, const ast::interface_expr&)

} // namespace ghoti::sema
