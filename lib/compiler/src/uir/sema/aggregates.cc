#include <algorithm>
#include <cstdint>
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
#include <stdx/utility.hh>

#include "compiler/arena.hh"
#include "compiler/ast/expression.hh"
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

// Keeps UIR's nominal keys apart from the old pipeline's, which key on a symbol table index
constexpr u64   NOMINAL_MARKER{0x7569725f6e6f6d69};
constexpr usize NOMINAL_TABLE_BASE{usize{1} << 48};

[[nodiscard]] auto default_name(gs::type_kind kind) -> std::string_view {
    switch (kind) {
    case gs::type_kind::UNION: return "union";
    case gs::type_kind::ENUM:  return "enum";
    default:                   return "struct";
    }
}

// The record's capture list, skipped. Captures make a type depend on the body it is written in
[[nodiscard]] auto skip_captures(record_reader& rec) -> u32 {
    const auto count{rec.count()};
    for (u32 i{0}; i < count; ++i) {
        DISCARD(rec.ref_field());
        DISCARD(rec.u32_field());
        DISCARD(rec.str_field());
    }
    return count;
}

// The AST expression a type instruction was lowered from, which may be written as a type
template <typename Expr>
[[nodiscard]] auto ast_of(const unit& u, const source& src) -> const Expr& {
    return src.visit(
        [&](const auto& id) -> const Expr& { return u.source->ast.template get_as<Expr>(id); });
}

// The old type payloads take the declaring module mutably, but nothing writes through it
[[nodiscard]] auto enclosing_of(const unit& u) -> mod::module_t& {
    return const_cast<mod::module_t&>(*u.source);
}

} // namespace

auto nominal_key_hash::operator()(const nominal_key& key) const noexcept -> u64 {
    stdx::hasher h{key.unit};
    h.combine(std::to_underlying(key.inst));
    for (const auto arg : key.args) { h.combine(std::to_underlying(arg)); }
    return h.finalize();
}

auto compilation::declare_nominal(const unit& u, inst_id inst, gs::type_kind kind) -> nominal& {
    nominal_key key{
        .unit = u.index,
        .inst = inst,
        .args = calls_.empty() ? std::vector<ip::index>{} : calls_.back().key->args,
    };
    if (const auto found{nominal_ids_.find(key)}; found != nominal_ids_.end()) {
        return *nominals_[found->second];
    }
    const auto             id{static_cast<u32>(nominals_.size())};
    const gs::types::key_t type_key{kind, gs::types::mut::CONSTANT, NOMINAL_MARKER, u64{id}};
    auto&                  type{*ctx_.pool[type_key]};
    type.set_symbol_table_idx(NOMINAL_TABLE_BASE + id);
    auto& n{*nominals_.emplace_back(stdx::make_box<nominal>(nominal{
        .owner       = &units_[u.index],
        .inst        = inst,
        .type        = &type,
        .name        = std::string{default_name(kind)},
        .state       = nominal_state::DECLARED,
        .in_call     = !calls_.empty(),
        .enum_values = {},
    }))};
    nominal_ids_.emplace(std::move(key), id);
    ctx_.user_type_names.insert_or_assign(&type, lasting_name(n.name));
    return n;
}

auto compilation::lasting_name(std::string_view name) -> std::string_view {
    // The context outlives the compilation, so the names it keeps live in its arena
    auto chars{make_uninit_span<char>(ctx_.arena, name.size())};
    std::ranges::copy(name, chars.begin());
    return {chars.data(), chars.size()};
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
    analyzer   a{*this, *n->owner};
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
    if (!is_comptime()) {
        return comp_.unsupported(u_, src_of(inst), "a type declared in a runtime body");
    }
    return interned_type(*comp_.declare_nominal(u_, inst, kind).type);
}

auto analyzer::resolve_fields(nominal& n) -> result<void> {
    switch (n.type->get_kind()) {
    case gs::type_kind::STRUCT: return resolve_struct(n);
    case gs::type_kind::UNION:  return resolve_union(n);
    default:                    return resolve_enum(n);
    }
}

auto analyzer::field_type_of(gsl::span<const inst_id> body,
                             std::string_view         name,
                             const source&            src) -> result<gsl::not_null<gs::type*>> {
    auto& type{*TRY(eval_type(body, src))};
    if (type.get_kind() == gs::type_kind::AUTO) {
        return comp_.unsupported(u_, src, "a field whose type is inferred");
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
    if ((agg_flags & flags::AGG_PACKED) != 0) {
        return comp_.unsupported(u_, src, "a packed struct");
    }
    if (skip_captures(rec) != 0) {
        return comp_.unsupported(u_, src, "a type that captures locals");
    }
    const auto& expr{ast_of<ast::struct_expr>(u_, src)};
    // The type's AST lists every gated field, not only the ones the configuration selects
    if (!expr.cfg_groups.empty()) { return comp_.unsupported(u_, src, "`@cfg`-gated fields"); }

    auto&      ctx{comp_.ctx()};
    const auto count{rec.count()};
    auto       field_types{ctx.pool.get_many_pending(count)};
    for (u32 i{0}; i < count; ++i) {
        const auto name{rec.str_field()};
        DISCARD(rec.u32_field());
        const auto type_body{rec.body_field()};
        DISCARD(rec.body_field());
        const auto  attrs{rec.body_field()};
        const auto& field_src{type_body.empty() ? src : src_of(type_body.back())};
        if (!attrs.empty()) { return comp_.unsupported(u_, field_src, "field attributes"); }
        field_types[i] = *TRY(field_type_of(type_body, name, field_src));
    }

    const gsl::span<const ast::struct_expr::field> fields{expr.fields};
    n.type->resolve<gs::types::struct_t>(field_types,
                                         fields,
                                         ctx.pool.get_many_pending(0),
                                         enclosing_of(u_),
                                         (agg_flags & flags::AGG_EXTERN) != 0,
                                         false,
                                         ctx.arena.make_span<u64>(count),
                                         gs::declared_names(ctx.arena, *u_.source, fields));
    return {};
}

auto analyzer::resolve_union(nominal& n) -> result<void> {
    auto        rec{record(n.inst)};
    const auto& src{src_of(n.inst)};
    const auto  agg_flags{rec.u32_field()};
    const auto  tag_kind{static_cast<union_tag>(rec.u32_field())};
    const auto  tag_body{rec.body_field()};
    if ((agg_flags & flags::AGG_PACKED) != 0) {
        return comp_.unsupported(u_, src, "a packed union");
    }
    if (skip_captures(rec) != 0) {
        return comp_.unsupported(u_, src, "a type that captures locals");
    }

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
    if (tag_enum) { return comp_.unsupported(u_, src, "a union tagged by its own enum"); }
    const auto& expr{ast_of<ast::union_expr>(u_, src)};
    // The type's AST lists every gated field, not only the ones the configuration selects
    if (!expr.cfg_groups.empty()) { return comp_.unsupported(u_, src, "`@cfg`-gated fields"); }

    auto&      ctx{comp_.ctx()};
    const auto count{rec.count()};
    auto       field_types{ctx.pool.get_many_pending(count)};
    for (u32 i{0}; i < count; ++i) {
        const auto  name{rec.str_field()};
        const auto  type_body{rec.body_field()};
        const auto  attrs{rec.body_field()};
        const auto& field_src{type_body.empty() ? src : src_of(type_body.back())};
        if (!attrs.empty()) { return comp_.unsupported(u_, field_src, "field attributes"); }
        field_types[i] = *TRY(field_type_of(type_body, name, field_src));
    }

    const bool                                    is_extern{(agg_flags & flags::AGG_EXTERN) != 0};
    const gsl::span<const ast::union_expr::field> fields{expr.fields};
    n.type->resolve<gs::types::union_t>(field_types,
                                        fields,
                                        ctx.pool.get_many_pending(0),
                                        enclosing_of(u_),
                                        is_extern || void_tag,
                                        is_extern,
                                        false,
                                        tag_enum,
                                        gs::declared_names(ctx.arena, *u_.source, fields));
    return {};
}

auto analyzer::resolve_enum(nominal& n) -> result<void> {
    auto        rec{record(n.inst)};
    const auto& src{src_of(n.inst)};
    const auto  enum_flags{rec.u32_field()};
    const auto  underlying_body{rec.body_field()};
    if (skip_captures(rec) != 0) {
        return comp_.unsupported(u_, src, "a type that captures locals");
    }

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
        DISCARD(rec.str_field());
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
        if (!small || *small > INT64_MAX || *small < INT64_MIN) {
            return comp_.unsupported(u_, src, "an enum value past 64 bits");
        }
        n.enum_values.emplace_back(static_cast<i64>(*small));
        next = next + big_int{i128{1}};
    }

    const auto& expr{ast_of<ast::enum_expr>(u_, src)};
    // The type's AST lists every gated field, not only the ones the configuration selects
    if (!expr.cfg_groups.empty()) { return comp_.unsupported(u_, src, "`@cfg`-gated fields"); }
    const gsl::span<const ast::enum_expr::enumeration> values{expr.enumerations};
    n.type->resolve<gs::types::enum_t>(values,
                                       (enum_flags & flags::NON_EXHAUSTIVE) != 0,
                                       underlying,
                                       ctx.pool.get_many_pending(0),
                                       enclosing_of(u_),
                                       gsl::span<const gsl::not_null<gs::type*>>{},
                                       gs::declared_names(ctx.arena, *u_.source, values));
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

auto analyzer::member_of(gs::type& owner, std::string_view name, const source& src)
    -> result<operand> {
    if (owner.get_kind() == gs::type_kind::ENUM) {
        TRY(comp_.require_fields(u_, src, owner));
        const auto& en{owner.get_data().as<gs::types::enum_t>()};
        if (std::ranges::contains(en.variant_names, name)) {
            return enum_constant(owner, name, src);
        }
    }
    const auto n{comp_.nominal_of(owner)};
    if (!n) { return comp_.unsupported(u_, src, "a member of a type UIR didn't declare"); }
    if (n->in_call) {
        return comp_.unsupported(u_, src, "a member of a type a comptime call created");
    }
    const auto& owner_mod{*n->owner->uir};
    if (const auto ns{owner_mod.namespace_of(n->inst)}) {
        for (const auto decl : ns->decls) {
            if (owner_mod.string(owner_mod.decl_at(decl).name) == name) {
                return comp_.analyze_decl(*n->owner, decl, src);
            }
        }
    }
    return comp_.report(u_,
                        src,
                        gs::error::UNDECLARED_IDENTIFIER,
                        fmt::format("Type '{}' has no field named '{}'",
                                    comp_.ctx().type_display_name(owner),
                                    name));
}

} // namespace ghoti::uir::sema
