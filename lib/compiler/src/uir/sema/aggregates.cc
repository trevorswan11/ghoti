#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/hash.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/arena.hh"
#include "compiler/ast/attributes.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/id.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/big_int.hh"
#include "support/int128.hh"

namespace ghoti::uir::sema {

namespace {

// Keeps nominal keys apart from the keys of types that carry a real symbol table index
constexpr u64   NOMINAL_MARKER{0x7569725f6e6f6d69};
constexpr usize NOMINAL_TABLE_BASE{usize{1} << 48};

// The items with each `@cfg` group's selected arm spliced in where AstGen put them
template <typename Item, typename Group>
[[nodiscard]] auto spliced_items(const std::vector<Item>&           items,
                                 const std::vector<Group>&          groups,
                                 gsl::span<const stdx::option<u32>> arms) -> std::vector<Item> {
    std::vector<Item> out{items};
    usize             offset{0};
    for (usize g{0}; g < groups.size() && g < arms.size(); ++g) {
        if (!arms[g]) { continue; }
        const auto& arm{groups[g].arms[*arms[g]]};
        const auto  at{static_cast<idiff>(groups[g].position + offset)};
        out.insert(out.begin() + at, arm.items.begin(), arm.items.end());
        offset += arm.items.size();
    }

    return out;
}

[[nodiscard]] auto default_name(gs::type_kind kind) -> std::string_view {
    switch (kind) {
    case gs::type_kind::UNION:     return "union";
    case gs::type_kind::ENUM:      return "enum";
    case gs::type_kind::INTERFACE: return "interface";
    default:                       return "struct";
    }
}

// The record's capture list, skipped. Captures make a type depend on the body it is written in
[[nodiscard]] auto skip_captures(record_reader& rec) -> u32 {
    const auto count{rec.count()};
    for (u32 i{0}; i < count; ++i) { rec.skip_ref().skip_u32().skip_str(); }

    return count;
}

// The AST expression a type instruction was lowered from, which may be written as a type
template <typename Expr>
[[nodiscard]] auto ast_of(const unit& u, const source& src) -> const Expr& {
    return src.visit(
        [&](const auto& id) -> const Expr& { return u.source->ast.template get_as<Expr>(id); });
}

} // namespace

auto nominal_key_hash::operator()(const nominal_key& key) const noexcept -> u64 {
    stdx::hasher h{key.unit};
    h.combine(std::to_underlying(key.inst));
    for (const auto arg : key.args) { h.combine(std::to_underlying(arg)); }
    return h.finalize();
}

auto compilation::instance_display_name(const instance& call) -> std::string {
    std::vector<std::string> args;
    for (const auto& binding : call.bindings) {
        if (!binding.value) { continue; }
        if (constants().is_type(*binding.value)) {
            args.emplace_back(ctx_.type_display_name(constants().type_at(*binding.value)));
            continue;
        }

        args.emplace_back(gir::const_value::from_interned(constants(), *binding.value).mangle());
    }

    const auto name{call.source_name.empty() ? std::string_view{call.symbol}
                                             : std::string_view{call.source_name}};
    return fmt::format("{}({})", name, fmt::join(args, ", "));
}

auto compilation::call_identity(const comptime_frame& frame) -> std::vector<ip::index> {
    // A generic's instance binds some parameters before the call, so they are part of it too
    std::vector<ip::index> args;
    for (const auto& binding : frame.key->fn->bindings) {
        if (binding.value) { args.emplace_back(*binding.value); }
    }

    args.insert(args.end(), frame.key->args.begin(), frame.key->args.end());
    return args;
}

auto compilation::declare_nominal(const unit&            u,
                                  inst_id                inst,
                                  gs::type_kind          kind,
                                  std::vector<ip::index> identity) -> nominal& {
    // Only a call begun inside the declaration being resolved owns what it declares
    const bool  in_call{calls_.size() > call_floor_};
    nominal_key key{
        .unit = u.index,
        .inst = inst,
        .args = in_call ? call_identity(calls_.back()) : std::vector<ip::index>{},
    };
    key.args.insert(key.args.end(), identity.begin(), identity.end());
    if (const auto found{nominal_ids_.find(key)}; found != nominal_ids_.end()) {
        return *nominals_[found->second];
    }

    const auto             id{static_cast<u32>(nominals_.size())};
    const gs::types::key_t type_key{kind, gs::types::mut::CONSTANT, NOMINAL_MARKER, u64{id}};
    auto&                  type{*ctx_.pool[type_key]};
    type.set_symbol_table_idx(NOMINAL_TABLE_BASE + id);
    auto& n{*nominals_.emplace_back(arena_.make<nominal>(nominal{
        .owner       = &units_[u.index],
        .inst        = inst,
        .type        = &type,
        .name        = std::string{default_name(kind)},
        .state       = nominal_state::DECLARED,
        .in_call     = in_call,
        .enum_values = {},
    }))};
    nominal_ids_.emplace(std::move(key), id);

    // A ctor instance's type is named for the call, and its members under the call's symbol
    if (in_call) {
        const auto& call{*calls_.back().key->fn};
        n.symbol_prefix = call.symbol;
        n.name          = instance_display_name(call);
        n.creator.emplace(call);
    }

    ctx_.user_type_names.insert_or_assign(&type, lasting_name(n.name));
    return n;
}

auto compilation::lasting_name(std::string_view name) -> std::string_view {
    // The context outlives the compilation, so the names it keeps live in its arena
    auto chars{make_uninit_span<char>(ctx_.arena, name.size())};
    std::ranges::copy(name, chars.begin());
    return {chars.data(), chars.size()};
}

auto compilation::nominal_number(const gs::type& t) -> usize {
    return t.get_symbol_table_idx() - NOMINAL_TABLE_BASE;
}

auto compilation::existing_nominal(const unit& u, inst_id inst) -> stdx::option<nominal&> {
    const nominal_key key{.unit = u.index, .inst = inst, .args = {}};
    if (const auto found{nominal_ids_.find(key)}; found != nominal_ids_.end()) {
        return *nominals_[found->second];
    }

    return stdx::none;
}

auto compilation::nominal_of(const gs::type& t) -> stdx::option<nominal&> {
    const auto table{t.get_symbol_table_idx_opt()};
    if (!table || *table < NOMINAL_TABLE_BASE || *table - NOMINAL_TABLE_BASE >= nominals_.size()) {
        return stdx::none;
    }

    return *nominals_[*table - NOMINAL_TABLE_BASE];
}

auto compilation::name_nominal(gs::type& t, std::string_view name) -> void {
    const auto n{nominal_of(t)};
    if (!n || n->name != default_name(t.get_kind())) { return; }
    n->name = std::string{name};
    ctx_.user_type_names.insert_or_assign(n->type.get(), lasting_name(n->name));
}

auto compilation::require_fields(const unit& u, const source& use, gs::type& t) -> result<void> {
    const auto n{nominal_of(t)};
    if (!n) { return {}; }
    switch (n->state) {
    case nominal_state::RESOLVED:
    case nominal_state::LAYOUT_RESOLVING: return {};
    case nominal_state::FAILED:           return stdx::err{stop::POISON};
    case nominal_state::FIELDS_RESOLVING:
        return report(u,
                      use,
                      gs::error::CYCLIC_DEPENDENCY,
                      fmt::format("'{}' is used during its own resolution", n->name));
    case nominal_state::DECLARED: break;
    }

    n->state = nominal_state::FIELDS_RESOLVING;
    analyzer a{*this, *n->owner};
    a.enter(*n);
    const auto resolved{a.resolve_fields(*n)};
    n->state = resolved ? nominal_state::RESOLVED : nominal_state::FAILED;
    if (!resolved) { return stdx::err{resolved.error()}; }

    // A qualified twin made before the fields resolved holds the payload from then
    for (const auto mut :
         {gs::types::mut::MUTABLE, gs::types::mut::VOLATILE, gs::types::mut::CONSTANT_VOLATILE}) {
        auto key{n->type->get_key()};
        key.set_mut(mut);
        auto& twin{*ctx_.pool[key]};
        twin.resolve<gs::type::data_t>(n->type->get_data());
        twin.copy_identity_from(*n->type);
    }

    return {};
}

auto analyzer::analyze_nominal_decl(inst_id inst, gs::type_kind kind) -> result<operand> {
    // A type declared in a runtime body is one type for each set of values it captures
    auto       captures{TRY(captured_values(inst, kind))};
    const auto identity{is_comptime() ? std::vector<ip::index>{} : captures};
    auto&      n{comp_.declare_nominal(u_, inst, kind, identity)};
    if (n.state == nominal_state::DECLARED && n.captures.empty()) {
        n.captures = std::move(captures);
    }

    return interned_type(*n.type);
}

auto analyzer::captured_values(inst_id inst, gs::type_kind kind) -> result<std::vector<ip::index>> {
    auto rec{record(inst)};
    rec.skip_u32();
    if (kind == gs::type_kind::UNION) { rec.skip_u32().skip_body(); }
    if (kind == gs::type_kind::ENUM) { rec.skip_body(); }

    const auto&            src{src_of(inst)};
    const auto             count{rec.count()};
    std::vector<ip::index> values;
    for (u32 i{0}; i < count; ++i) {
        const auto value_ref{*rec.ref_field()};
        rec.skip_u32().skip_str();
        const auto value{TRY(resolve_value(value_ref, src))};
        const auto index{comptime_value(value)};
        if (!index) { return comp_.unsupported(u_, src, "a type that captures a runtime value"); }
        values.emplace_back(*index);
    }

    return values;
}

auto analyzer::resolve_fields(nominal& n) -> result<void> {
    switch (n.type->get_kind()) {
    case gs::type_kind::STRUCT: return resolve_struct(n);
    case gs::type_kind::UNION:  return resolve_union(n);
    default:                    return resolve_enum(n);
    }
}

auto analyzer::field_type_of(gsl::span<const inst_id>               body,
                             stdx::option<gsl::span<const inst_id>> default_body,
                             std::string_view                       name,
                             const source& src) -> result<gsl::not_null<gs::type*>> {
    auto& type{*TRY(eval_type(body, src))};
    if (type.get_kind() == gs::type_kind::AUTO) {
        if (!default_body || default_body->empty()) {
            return comp_.report(u_,
                                src,
                                gs::error::ILLEGAL_AUTO_USAGE,
                                fmt::format("{} field '{}' cannot have type 'auto'",
                                            default_body ? "Struct" : "Union",
                                            name));
        }

        const auto value{TRY(eval_comptime(*default_body))};
        return &inferred_type(value);
    }

    // A by-value field of a type still resolving its own fields can never be sized
    if (type.get_kind() == gs::type_kind::STRUCT || type.get_kind() == gs::type_kind::UNION) {
        if (const auto inner{comp_.nominal_of(type)};
            inner && inner->state == nominal_state::FIELDS_RESOLVING) {
            return comp_.report(
                u_,
                src,
                gs::error::CYCLIC_DEPENDENCY,
                fmt::format("Field '{}' has an incomplete type; creates an infinite size cycle",
                            name));
        }

        TRY(comp_.require_fields(u_, src, type));
    }

    return &type;
}

auto analyzer::resolve_struct(nominal& n) -> result<void> {
    auto        rec{record(n.inst)};
    const auto& src{src_of(n.inst)};
    const auto  agg_flags{rec.u32_field()};
    const bool  is_extern{(agg_flags & flags::AGG_EXTERN) != 0};
    const bool  is_packed{(agg_flags & flags::AGG_PACKED) != 0};
    const bool  bit_packed{is_packed && !is_extern};
    DISCARD(skip_captures(rec));

    const auto& expr{ast_of<ast::struct_expr>(u_, src)};

    // The type's AST lists every gated field, not only the ones the configuration selects
    const gsl::span<const ast::struct_expr::field> fields{
        expr.cfg_groups.empty() ? gsl::span<const ast::struct_expr::field>{expr.fields}
                                : comp_.keep_items(spliced_items(
                                      expr.fields, expr.cfg_groups, mod().field_arms_of(n.inst)))};

    auto&      ctx{comp_.ctx()};
    const auto count{rec.count()};
    auto       field_types{ctx.pool.get_many_pending(count)};
    auto       alignments{ctx.arena.make_span<u64>(count)};
    for (u32 i{0}; i < count; ++i) {
        const auto name{rec.str_field()};
        rec.skip_u32();
        const auto  type_body{rec.body_field()};
        const auto  default_body{rec.body_field()};
        const auto  attrs{rec.body_field()};
        const auto& field_src{type_body.empty() ? src : src_of(type_body.back())};
        const auto  resolved{TRY(fold_attributes(attrs, {.target = ast::attribute_target::FIELD}))};
        alignments[i]  = resolved.alignment.value_or(0);
        field_types[i] = *TRY(field_type_of(type_body, default_body, name, field_src));
        if (bit_packed) {
            TRY(check_packed_field(*field_types[i], name, "struct", field_src));
            if (resolved.alignment) { TRY(reject_packed_alignment(name, "struct", field_src)); }
        }
    }

    if (bit_packed) {
        u64 total{0};
        for (const auto field : field_types) {
            total += gs::packed_field_bits(*field, comp_.ptr_bits()).value_or(0);
        }

        TRY(check_packed_width(total, "struct", src));
    }

    n.type->resolve<gs::types::struct_t>(field_types,
                                         fields,
                                         ctx.pool.get_many_pending(0),
                                         *u_.source,
                                         is_extern,
                                         is_packed,
                                         alignments,
                                         gs::declared_names(ctx.arena, *u_.source, fields));
    for (usize i{0}; i < fields.size() && i < count; ++i) {
        note_declared_name(ast::node_id{fields[i].name}, *field_types[i]);
    }
    return {};
}

auto analyzer::resolve_union(nominal& n) -> result<void> {
    auto        rec{record(n.inst)};
    const auto& src{src_of(n.inst)};
    const auto  agg_flags{rec.u32_field()};
    const auto  tag_kind{static_cast<union_tag>(rec.u32_field())};
    const auto  tag_body{rec.body_field()};
    const bool  is_extern{(agg_flags & flags::AGG_EXTERN) != 0};
    const bool  is_packed{(agg_flags & flags::AGG_PACKED) != 0};
    const bool  bit_packed{is_packed && !is_extern};
    DISCARD(skip_captures(rec));

    stdx::option<gs::type&> tag_enum;
    bool                    void_tag{false};
    if (tag_kind == union_tag::EXPLICIT) {
        const auto& tag_src{tag_body.empty() ? src : src_of(tag_body.back())};
        auto&       tag{*TRY(eval_type(tag_body, tag_src))};
        if (tag.get_kind() == gs::type_kind::VOID_) {
            void_tag = true;
        } else if (tag.get_kind() == gs::type_kind::ENUM) {
            TRY(comp_.require_fields(u_, tag_src, tag));
            tag_enum = tag;
        } else {
            return comp_.report(
                u_,
                tag_src,
                gs::error::ILLEGAL_UNION_TAG,
                fmt::format("A union's tag must be an enum, or `void` for no tag; found '{}'",
                            comp_.ctx().type_display_name(tag)));
        }
    }

    const auto& expr{ast_of<ast::union_expr>(u_, src)};

    // The type's AST lists every gated field, not only the ones the configuration selects
    const gsl::span<const ast::union_expr::field> fields{
        expr.cfg_groups.empty() ? gsl::span<const ast::union_expr::field>{expr.fields}
                                : comp_.keep_items(spliced_items(
                                      expr.fields, expr.cfg_groups, mod().field_arms_of(n.inst)))};

    auto&      ctx{comp_.ctx()};
    const auto count{rec.count()};
    auto       field_types{ctx.pool.get_many_pending(count)};
    for (u32 i{0}; i < count; ++i) {
        const auto  name{rec.str_field()};
        const auto  type_body{rec.body_field()};
        const auto  attrs{rec.body_field()};
        const auto& field_src{type_body.empty() ? src : src_of(type_body.back())};
        const auto  resolved{TRY(fold_attributes(attrs, {.target = ast::attribute_target::FIELD}))};
        field_types[i] = *TRY(field_type_of(type_body, stdx::none, name, field_src));
        if (bit_packed) { TRY(check_packed_field(*field_types[i], name, "union", field_src)); }
        if (resolved.alignment) {
            DISCARD(comp_.report(u_,
                                 source{fields[i].name},
                                 gs::error::ILLEGAL_ATTRIBUTE,
                                 "Attribute 'align' is not supported on union fields"));
            if (bit_packed) { TRY(reject_packed_alignment(name, "union", field_src)); }
        }
    }

    if (bit_packed) {
        u64 widest{0};
        for (const auto field : field_types) {
            widest =
                std::max<u64>(widest, gs::packed_field_bits(*field, comp_.ptr_bits()).value_or(0));
        }

        TRY(check_packed_width(widest, "union", src));
    }

    // Every field names a variant of the tag enum, and every variant has a field
    if (tag_enum) {
        const auto& en{tag_enum->get_data().as<gs::types::enum_t>()};
        for (usize i{0}; i < count; ++i) {
            const auto name{u_.source->ast.get_as<ast::identifier_expr>(fields[i].name).name};
            if (!std::ranges::contains(en.variant_names, name)) {
                return comp_.report(
                    u_,
                    source{fields[i].name},
                    gs::error::ILLEGAL_UNION_TAG,
                    fmt::format("Union field '{}' is not a variant of its tag enum '{}'",
                                name,
                                ctx.type_display_name(*tag_enum)));
            }
        }

        for (const auto variant : en.variant_names) {
            const bool has_field{std::ranges::any_of(fields, [&](const auto& field) {
                return u_.source->ast.get_as<ast::identifier_expr>(field.name).name == variant;
            })};
            if (!has_field) {
                return comp_.report(
                    u_,
                    src,
                    gs::error::ILLEGAL_UNION_TAG,
                    fmt::format("Variant '{}' of the tag enum '{}' has no union field",
                                variant,
                                ctx.type_display_name(*tag_enum)));
            }
        }
    }

    n.type->resolve<gs::types::union_t>(field_types,
                                        fields,
                                        ctx.pool.get_many_pending(0),
                                        *u_.source,
                                        is_extern || is_packed || void_tag,
                                        is_extern,
                                        is_packed,
                                        tag_enum,
                                        gs::declared_names(ctx.arena, *u_.source, fields));
    for (usize i{0}; i < fields.size() && i < count; ++i) {
        note_declared_name(ast::node_id{fields[i].name}, *field_types[i]);
    }
    return {};
}

auto analyzer::resolve_enum(nominal& n) -> result<void> {
    auto        rec{record(n.inst)};
    const auto& src{src_of(n.inst)};
    const auto  enum_flags{rec.u32_field()};
    const auto  underlying_body{rec.body_field()};
    DISCARD(skip_captures(rec));

    auto& ctx{comp_.ctx()};
    auto& underlying{underlying_body.empty()
                         ? ctx.get_int(32, true)
                         : *TRY(eval_type(underlying_body, src_of(underlying_body.back())))};
    if (!gs::is_integer(underlying.get_kind())) {
        return comp_.report(u_,
                            src_of(underlying_body.back()),
                            gs::error::TYPE_MISMATCH,
                            fmt::format("An enum's backing type must be an integer; found '{}'",
                                        ctx.type_display_name(underlying)));
    }

    const auto count{rec.count()};
    big_int    next{i128{0}};
    n.enum_values.clear();
    for (u32 i{0}; i < count; ++i) {
        rec.skip_str();
        const auto value_body{rec.body_field()};
        if (!value_body.empty()) {
            const auto& value_src{src_of(value_body.back())};
            const auto  value{TRY(eval_comptime(value_body))};
            const auto  typed{
                TRY(coerce_to(value, underlying, value_src, {.what = use_site::kind::STORE}))};
            next = *int_of(comp_.constants().value_at(TRY(require_comptime(typed, value_src))));
        } else if (!gs::comptime_int_fits(next, underlying, comp_.ptr_bits())) {
            return comp_.report(u_,
                                src,
                                gs::error::LITERAL_OUT_OF_RANGE,
                                messages::value_out_of_range(ctx, next, underlying));
        }

        const auto small{next.to_i128()};
        if (!small || *small > std::numeric_limits<i64>::max() ||
            *small < std::numeric_limits<i64>::min()) {
            return comp_.unsupported(u_, src, "an enum value past 64 bits");
        }

        n.enum_values.emplace_back(static_cast<i64>(*small));
        next = next + big_int{i128{1}};
    }

    const auto& expr{ast_of<ast::enum_expr>(u_, src)};

    // The type's AST lists every gated field, not only the ones the configuration selects
    const gsl::span<const ast::enum_expr::enumeration> values{
        expr.cfg_groups.empty()
            ? gsl::span<const ast::enum_expr::enumeration>{expr.enumerations}
            : comp_.keep_items(
                  spliced_items(expr.enumerations, expr.cfg_groups, mod().field_arms_of(n.inst)))};
    n.type->resolve<gs::types::enum_t>(values,
                                       (enum_flags & flags::NON_EXHAUSTIVE) != 0,
                                       underlying,
                                       ctx.pool.get_many_pending(0),
                                       *u_.source,
                                       gsl::span<const gsl::not_null<gs::type*>>{},
                                       gs::declared_names(ctx.arena, *u_.source, values));
    for (const auto& value : values) { note_declared_name(ast::node_id{value.name}, *n.type); }
    return {};
}

auto analyzer::enum_constant(gs::type& enum_type, std::string_view name, const source& src)
    -> result<operand> {
    TRY(comp_.require_fields(u_, src, enum_type));
    const auto& en{enum_type.get_data().as<gs::types::enum_t>()};
    const auto  n{comp_.nominal_of(enum_type)};
    for (usize i{0}; i < en.variant_names.size(); ++i) {
        if (en.variant_names[i] != name) { continue; }
        if (!n) { return comp_.unsupported(u_, src, "a variant of an enum UIR didn't declare"); }
        return comp_.constants().intern(n->enum_values[i], enum_type);
    }

    return comp_.report(u_,
                        src,
                        gs::error::UNDECLARED_IDENTIFIER,
                        fmt::format("Type '{}' has no field named '{}'",
                                    comp_.ctx().type_display_name(enum_type),
                                    name));
}

auto analyzer::variant_name(ip::index value) -> stdx::option<std::string_view> {
    const auto& v{comp_.constants().value_at(value)};
    const auto  number{int_of(v)};
    const auto  n{v.type ? comp_.nominal_of(*v.type) : stdx::none};
    const auto  small{number ? number->to_i128() : stdx::none};
    if (!n || !small || v.type->get_kind() != gs::type_kind::ENUM) { return stdx::none; }
    const auto at{std::ranges::find(n->enum_values, static_cast<i64>(*small))};
    if (at == n->enum_values.end()) { return stdx::none; }
    const auto& names{v.type->get_data().as<gs::types::enum_t>().variant_names};
    return names[static_cast<usize>(at - n->enum_values.begin())];
}

auto analyzer::member_of(gs::type& owner, std::string_view name, const source& src)
    -> result<operand> {
    if (owner.get_kind() == gs::type_kind::ENUM) {
        TRY(comp_.require_fields(u_, src, owner));
        const auto& en{owner.get_data().as<gs::types::enum_t>()};
        if (const auto at{std::ranges::find(en.variant_names, name)};
            at != en.variant_names.end()) {
            const auto index{static_cast<usize>(at - en.variant_names.begin())};
            if (index < en.ast_enumerations.size()) {
                note_member(src,
                            en.enclosing,
                            ast::node_id{en.ast_enumerations[index].name},
                            stdx::none,
                            stdx::none,
                            owner);
            }

            return enum_constant(owner, name, src);
        }
    }

    // A builtin type has only the members impls add to it
    const auto n{comp_.nominal_of(owner)};
    if (!n) {
        const auto impls{TRY(comp_.impls_of(owner))};
        if (impls.empty()) {
            return comp_.unsupported(u_, src, "a member of a type UIR didn't declare");
        }
    }

    // A use from another module is located at what it reaches in that module
    const auto ns{n ? n->owner->uir->namespace_of(n->inst) : stdx::option<const namespace_info&>{}};
    if (ns) {
        const auto& owner_mod{*n->owner->uir};
        for (const auto decl : ns->decls) {
            if (owner_mod.string(owner_mod.decl_at(decl).name) == name) {
                warn_deprecated_decl(*n->owner, decl, src);
                note_member_decl(src, *n->owner, decl);
                const bool here{n->owner.get() == &u_};
                return comp_.analyze_member(
                    *n, decl, here ? src : source{*owner_mod.decl_at(decl).node});
            }
        }
    }

    // A member an impl adds to the type
    for (const auto applied : TRY(comp_.impls_of(owner))) {
        const auto& impl_unit{*applied->entry->owner};
        const auto& use{&impl_unit == &u_ ? src : impl_unit.uir->src_of(applied->entry->inst)};
        if (const auto member{TRY(comp_.impl_member(*applied, name, use))}) { return *member; }
    }

    return comp_.report(u_,
                        src,
                        gs::error::UNDECLARED_IDENTIFIER,
                        fmt::format("Type '{}' has no field named '{}'",
                                    comp_.ctx().type_display_name(owner),
                                    name));
}

} // namespace ghoti::uir::sema
