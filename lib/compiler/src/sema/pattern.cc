#include "compiler/sema/pattern.hh"

#include <algorithm>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/span>
#include <stdx/profiler.hh>

#include "compiler/ast/ast.hh"
#include "compiler/gir/layout.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"

namespace ghoti::sema::pattern {

namespace {

constexpr usize MAX_DECISION_NODES{200'000};

[[nodiscard]] auto make_node(node_kind kind, stdx::option<ast::node_id> source, type& value_type)
    -> node_ptr {
    auto out{std::make_unique<node>(node{.source = source, .value_type = &value_type})};
    out->kind = kind;
    return out;
}

// The range of values an integer column can hold, or none for an unbounded `comptime_int`
struct int_bounds {
    big_int lo;
    big_int hi;
};

[[nodiscard]] auto bounds_of(const type& t, u32 ptr_bits) -> stdx::option<int_bounds> {
    u64  bits{0};
    bool is_signed{false};
    if (const auto info{as_integer(t)}) {
        bits      = info->bits;
        is_signed = info->is_signed;
    } else if (t.get_kind() == type_kind::USIZE) {
        bits = ptr_bits;
    } else if (t.get_kind() == type_kind::ISIZE) {
        bits      = ptr_bits;
        is_signed = true;
    } else {
        return stdx::none;
    }
    if (bits == 0) { return int_bounds{.lo = big_int{i128{0}}, .hi = big_int{i128{0}}}; }
    const auto one{big_int{i128{1}}};
    if (!is_signed) { return int_bounds{.lo = big_int{i128{0}}, .hi = one.shl(bits) - one}; }
    return int_bounds{.lo = -one.shl(bits - 1), .hi = one.shl(bits - 1) - one};
}

[[nodiscard]] auto is_int_column(const type& t, u32 ptr_bits) -> bool {
    return t.get_kind() == type_kind::COMPTIME_INT || bounds_of(t, ptr_bits).has_value();
}

[[nodiscard]] auto union_field_name(const types::union_t& u, usize i) -> std::string_view {
    return u.enclosing.ast.get_as<ast::identifier_expr>(u.ast_fields[i].name).name;
}

[[nodiscard]] auto enum_variant_name(const types::enum_t& e, usize i) -> std::string_view {
    return e.enclosing.ast.get_as<ast::identifier_expr>(e.ast_enumerations[i].name).name;
}

// An implicit `.name`, or the member of a qualified `Type.name`
struct named_member {
    std::string_view           name;
    stdx::option<ast::node_id> qualifier;
};

[[nodiscard]] auto member_named_by(const ast::AST& tree, ast::node_id id)
    -> stdx::option<named_member> {
    if (const auto implicit{tree.get_as_opt<ast::implicit_access_expr>(id)}) {
        return named_member{tree.get_as<ast::identifier_expr>(implicit->member).name, stdx::none};
    }
    if (const auto dot{tree.get_as_opt<ast::dot_expr>(id)}) {
        return named_member{tree.get_as<ast::identifier_expr>(dot->member).name, *dot->object};
    }
    return stdx::none;
}

class elaborator {
  public:
    explicit elaborator(const elaboration_context& cx) : cx_{cx}, tree_{cx.mod.ast} {}

    auto run(ast::node_id id, type& deferred_column, bool place_is_const, bool top_level)
        -> stdx::result<node_ptr, diagnostic> {
        PROFILE_FUNCTION();
        auto& column{cx_.callbacks.concrete(deferred_column)};
        // A `&T` is transparent to everything except a capture of the reference itself
        if (const auto ref{column.get_data().as_opt<types::reference>()};
            ref && !tree_.get_as_opt<ast::capture_pattern_expr>(id) &&
            !tree_[id].is<ast::discarded>()) {
            if (const auto bound{tree_.get_as_opt<ast::bound_pattern_expr>(id)}) {
                auto inner{TRY(run(*bound->pattern, column, place_is_const, top_level))};
                inner->captures.emplace_back(bound->capture, column, place_is_const, false);
                return inner;
            }
            auto& referent{cx_.callbacks.concrete(ref->underlying)};
            auto  sub{TRY(run(id, referent, column.is_constant(), top_level))};
            auto  out{make_node(node_kind::DEREF, id, column)};
            out->children.emplace_back(0, "", referent, false, std::move(sub));
            return out;
        }

        if (tree_[id].is<ast::discarded>()) {
            return make_node(node_kind::WILDCARD, stdx::none, column);
        }
        // `.a .b` (a missing comma) parses as a member of the variant `.a`
        const auto call_target{tree_.get_as_opt<ast::call_expr>(id)};
        if (const auto dot{
                tree_.get_as_opt<ast::dot_expr>(call_target ? *call_target->function : id)};
            dot && tree_.get_as_opt<ast::implicit_access_expr>(dot->object)) {
            return make_sema_err("A variant pattern has no members; separate patterns with a "
                                 "comma (`.a, .b`)",
                                 error::ILLEGAL_MATCH_PATTERN,
                                 location_of(id));
        }
        if (const auto capture{tree_.get_as_opt<ast::capture_pattern_expr>(id)}) {
            auto out{make_node(node_kind::WILDCARD, id, column)};
            if (!capture->is_bare_discard && capture->capture.payload.is<ast::identifier_expr>()) {
                out->captures.emplace_back(capture->capture, column, place_is_const, false);
            }
            return out;
        }
        if (const auto bound{tree_.get_as_opt<ast::bound_pattern_expr>(id)}) {
            return run_bound(*bound, column, place_is_const, top_level);
        }
        if (const auto range{tree_.get_as_opt<ast::range_expr>(id)}) {
            return run_range(id, *range, column, top_level);
        }
        if (const auto init{tree_.get_as_opt<ast::initializer_expr>(id)}) {
            return run_aggregate(id, *init, column, place_is_const);
        }

        // `.ok(...)` / `Shape.circle(...)` and a bare `.ok` name variants of a union
        if (const auto union_data{column.get_data().as_opt<types::union_t>()}) {
            return run_variant(id, *union_data, column, place_is_const);
        }
        return run_value(id, column, top_level);
    }

  private:
    [[nodiscard]] auto location_of(ast::node_id id) const -> source_location {
        return tree_.location_of(id);
    }

    auto run_bound(const ast::bound_pattern_expr& bound,
                   type&                          column,
                   bool                           place_is_const,
                   bool                           top_level) -> stdx::result<node_ptr, diagnostic> {
        // `.ok |x|` reads like `.ok => |x|`, but would bind the whole union
        if (const auto union_data{column.get_data().as_opt<types::union_t>()}) {
            if (const auto member{member_named_by(tree_, *bound.pattern)}) {
                const auto& table{cx_.ctx.registry.get(column.get_symbol_table_idx())};
                const auto  proxy{table.get_proxy_opt(member->name)};
                if (proxy && proxy->index < union_data->fields.size() &&
                    union_data->type_at(proxy->index).get_kind() != type_kind::VOID_) {
                    return make_sema_err(
                        fmt::format("'.{0} |x|' would bind the whole union; write '.{0}(|x|)' for "
                                    "the payload or '.{0}(_) |x|' for the union",
                                    member->name),
                        error::ILLEGAL_MATCH_PATTERN,
                        location_of(*bound.pattern));
                }
            }
        }
        auto out{TRY(run(*bound.pattern, column, place_is_const, top_level))};
        if (bound.capture.payload.is<ast::identifier_expr>()) {
            out->captures.emplace_back(bound.capture, column, place_is_const, false);
        }
        return out;
    }

    auto run_variant(ast::node_id          id,
                     const types::union_t& union_data,
                     type&                 column,
                     bool                  place_is_const) -> stdx::result<node_ptr, diagnostic> {
        if (union_data.is_untagged) {
            return make_sema_err("Cannot match on an untagged union; it has no runtime tag",
                                 error::TYPE_MISMATCH,
                                 location_of(id));
        }

        const auto call{tree_.get_as_opt<ast::call_expr>(id)};
        const auto member{member_named_by(tree_, call ? *call->function : id)};
        if (!member) {
            return make_sema_err("Match arm may only name a variant of the union in this context",
                                 error::ILLEGAL_MATCH_PATTERN,
                                 location_of(id));
        }
        TRY(check_qualifier(member->qualifier, column));

        const auto& table{cx_.ctx.registry.get(column.get_symbol_table_idx())};
        const auto  proxy{table.get_proxy_opt(member->name)};
        if (!proxy || proxy->index >= union_data.fields.size()) {
            return make_sema_err(
                fmt::format("Match expression contains unknown union field: {}", member->name),
                error::UNKNOWN_FIELD,
                location_of(id));
        }

        auto& payload{cx_.callbacks.concrete(union_data.type_at(proxy->index))};
        auto  out{make_node(node_kind::VARIANT, id, column)};
        out->index = proxy->index;
        out->name  = member->name;
        const bool is_void{payload.get_kind() == type_kind::VOID_};
        if (!call) {
            if (!is_void) {
                out->children.emplace_back(proxy->index,
                                           member->name,
                                           payload,
                                           false,
                                           make_node(node_kind::WILDCARD, stdx::none, payload));
            }
            return out;
        }

        if (is_void) {
            return make_sema_err(fmt::format("Variant '{}' has no payload to match", member->name),
                                 error::ILLEGAL_MATCH_PATTERN,
                                 location_of(id));
        }
        const auto arg{call->arguments.size() == 1
                           ? call->arguments.front().template as_opt<ast::expr_handle>()
                           : stdx::none};
        if (!arg) {
            return make_sema_err(
                fmt::format("Variant '{}' takes exactly one pattern for its payload", member->name),
                error::ILLEGAL_MATCH_PATTERN,
                location_of(id));
        }
        auto sub{TRY(run(**arg, payload, place_is_const, false))};
        out->children.emplace_back(proxy->index, member->name, payload, false, std::move(sub));
        return out;
    }

    auto check_qualifier(stdx::option<ast::node_id> qualifier, type& column)
        -> stdx::result<void, diagnostic> {
        if (!qualifier || !cx_.callbacks.denoted_type_of) { return {}; }
        const auto named{cx_.callbacks.denoted_type_of(*qualifier)};
        if (named && !is_same_unqualified(*named, column)) {
            return make_sema_err(fmt::format("This pattern names '{}', but the value is a '{}'",
                                             cx_.ctx.type_display_name(*named),
                                             cx_.ctx.type_display_name(column)),
                                 error::ILLEGAL_MATCH_PATTERN,
                                 location_of(*qualifier));
        }
        return {};
    }

    auto run_aggregate(ast::node_id                 id,
                       const ast::initializer_expr& init,
                       type&                        column,
                       bool place_is_const) -> stdx::result<node_ptr, diagnostic> {
        if (init.object_type) { TRY(check_qualifier(**init.object_type, column)); }
        const auto named_count{std::ranges::count_if(
            init.initializers, [](const auto& entry) { return entry.member.has_value(); })};
        const bool is_positional{named_count == 0 && !init.initializers.empty()};

        if (const auto arr{column.get_data().as_opt<types::array>()}) {
            if (named_count != 0) {
                return make_sema_err("An array pattern lists its elements by position",
                                     error::ILLEGAL_MATCH_PATTERN,
                                     location_of(id));
            }
            if (init.initializers.size() != arr->len) {
                return make_sema_err(fmt::format("Pattern has {} elements, but the array has {}",
                                                 init.initializers.size(),
                                                 arr->len),
                                     error::ILLEGAL_MATCH_PATTERN,
                                     location_of(id));
            }
            auto  out{make_node(node_kind::ARRAY, id, column)};
            auto& element{cx_.callbacks.concrete(arr->underlying)};
            for (usize i{0}; i < init.initializers.size(); ++i) {
                auto sub{TRY(run(*init.initializers[i].value, element, place_is_const, false))};
                out->children.emplace_back(i, "", element, false, std::move(sub));
            }
            return out;
        }

        const auto struct_data{column.get_data().as_opt<types::struct_t>()};
        const auto is_slice{column.get_kind() == type_kind::SLICE};
        if (!struct_data && !is_slice) {
            return make_sema_err(fmt::format("Cannot destructure a value of type '{}'",
                                             cx_.ctx.type_display_name(column)),
                                 error::ILLEGAL_MATCH_PATTERN,
                                 location_of(id));
        }
        if (is_positional) {
            return make_sema_err("A struct pattern names its fields: `.{ .x = ... }`",
                                 error::ILLEGAL_MATCH_PATTERN,
                                 location_of(id));
        }

        auto                       out{make_node(node_kind::RECORD, id, column)};
        std::set<std::string_view> seen;
        for (const auto& entry : init.initializers) {
            if (!entry.member) {
                return make_sema_err("A struct pattern names its fields: `.{ .x = ... }`",
                                     error::ILLEGAL_MATCH_PATTERN,
                                     location_of(*entry.value));
            }
            const auto& access{tree_.get_as<ast::implicit_access_expr>(*entry.member)};
            const auto  field_name{tree_.get_as<ast::identifier_expr>(access.member).name};
            if (!seen.insert(field_name).second) {
                return make_sema_err(
                    fmt::format("Field '{}' appears twice in this pattern", field_name),
                    error::DUPLICATE_FIELD,
                    location_of(*entry.value));
            }

            if (is_slice) {
                TRY(add_slice_field(*out, column, field_name, *entry.value, place_is_const));
            } else {
                TRY(add_struct_field(
                    *out, column, *struct_data, field_name, *entry.value, place_is_const));
            }
        }
        return out;
    }

    auto add_slice_field(node&            out,
                         type&            column,
                         std::string_view field_name,
                         ast::node_id     value,
                         bool             place_is_const) -> stdx::result<void, diagnostic> {
        const auto slice_data{column.get_data().as<types::slice>()};
        if (field_name != "len" && field_name != "ptr") {
            return make_sema_err(fmt::format("A slice has no field '{}'", field_name),
                                 error::UNKNOWN_FIELD,
                                 location_of(value));
        }
        auto& field_type{field_name == "len"
                             ? cx_.ctx.get_builtin_resolved_type(type_kind::USIZE)
                             : cx_.ctx.get_pointer(types::mut::CONSTANT, slice_data.underlying)};
        auto  sub{TRY(run(value, field_type, place_is_const, false))};
        out.children.emplace_back(field_name == "len" ? gir::SLICE_LEN_FIELD_INDEX
                                                      : gir::SLICE_PTR_FIELD_INDEX,
                                  field_name,
                                  field_type,
                                  false,
                                  std::move(sub));
        return {};
    }

    auto add_struct_field(node&                  out,
                          type&                  column,
                          const types::struct_t& struct_data,
                          std::string_view       field_name,
                          ast::node_id           value,
                          bool                   place_is_const) -> stdx::result<void, diagnostic> {
        const auto& table{cx_.ctx.registry.get(column.get_symbol_table_idx())};
        const auto  proxy{table.get_proxy_opt(field_name)};
        if (!proxy || proxy->index >= struct_data.fields.size()) {
            return make_sema_err(fmt::format("'{}' has no field '{}'",
                                             cx_.ctx.type_display_name(column),
                                             field_name),
                                 error::UNKNOWN_FIELD,
                                 location_of(value));
        }
        if (&struct_data.enclosing != &cx_.mod &&
            !struct_data.ast_fields[proxy->index].is_public()) {
            return make_sema_err(fmt::format("Field '{}' of '{}' is private",
                                             field_name,
                                             cx_.ctx.type_display_name(column)),
                                 error::ILLEGAL_PRIVATE_ACCESS,
                                 location_of(value));
        }
        auto& field_type{cx_.callbacks.concrete(struct_data.type_at(proxy->index))};
        auto  sub{TRY(run(value, field_type, place_is_const, false))};
        // Everything this field's sub-pattern binds lives in bit-packed storage
        const bool bit_packed{struct_data.is_bit_packed()};
        if (bit_packed) { mark_bit_packed(*sub); }
        out.children.emplace_back(proxy->index, field_name, field_type, bit_packed, std::move(sub));
        return {};
    }

    static auto mark_bit_packed(node& n) -> void {
        for (auto& site : n.captures) { site.place_is_bit_packed = true; }
        for (auto& sub : n.children) { mark_bit_packed(*sub.pattern); }
    }

    auto run_range(ast::node_id id, const ast::range_expr& range, type& column, bool top_level)
        -> stdx::result<node_ptr, diagnostic> {
        if (!is_int_column(column, cx_.target_ptr_bits)) {
            return make_sema_err(
                "Range patterns are only valid when matching on an integer or byte value",
                error::ILLEGAL_MATCH_PATTERN,
                location_of(id));
        }
        for (const auto bound : {range.lhs, range.rhs}) {
            if (bound && cx_.callbacks.resolve_leaf) {
                if (auto diag{cx_.callbacks.resolve_leaf(**bound, column)}) {
                    return stdx::err{std::move(*diag)};
                }
            }
        }

        const auto bounds{bounds_of(column, cx_.target_ptr_bits)};
        if (!bounds && (!range.lhs || !range.rhs)) {
            return make_sema_err("An open-ended range needs a value with a fixed integer type",
                                 error::ILLEGAL_MATCH_PATTERN,
                                 location_of(id));
        }
        const auto folded_bound{[&](stdx::option<ast::expr_handle> bound) -> stdx::option<big_int> {
            if (!bound) { return stdx::none; }
            const auto folded{cx_.callbacks.fold(**bound)};
            return folded ? folded->as_big_int_opt() : stdx::none;
        }};
        const auto lo{folded_bound(range.lhs)};
        const auto hi{folded_bound(range.rhs)};
        if ((range.lhs && !lo) || (range.rhs && !hi)) {
            if (!top_level) {
                return make_sema_err("A nested pattern must be known at compile time",
                                     error::ILLEGAL_MATCH_PATTERN,
                                     location_of(id));
            }
            return make_node(node_kind::OPAQUE, id, column);
        }

        const auto one{big_int{i128{1}}};
        const bool inclusive{id.get_token_type() == syntax::token_type_t::DOT_DOT_EQ};
        auto       out{make_node(node_kind::INT_RANGE, id, column)};
        out->lo = lo ? *lo : bounds->lo;
        out->hi = hi ? (inclusive ? *hi : *hi - one) : bounds->hi;
        if (out->hi < out->lo) {
            return make_sema_err("Range pattern is empty; its lower bound exceeds its upper bound",
                                 error::ILLEGAL_MATCH_PATTERN,
                                 location_of(id));
        }
        return out;
    }

    auto run_value(ast::node_id id, type& column, bool top_level)
        -> stdx::result<node_ptr, diagnostic> {
        if (column.get_data().is<types::enum_t>()) {
            if (const auto implicit{tree_.get_as_opt<ast::implicit_access_expr>(id)}) {
                const auto  name{tree_.get_as<ast::identifier_expr>(implicit->member).name};
                const auto& table{cx_.ctx.registry.get(column.get_symbol_table_idx())};
                if (!table.has(name)) {
                    return make_sema_err(
                        fmt::format("Match expression contains unknown enumeration: {}", name),
                        error::UNKNOWN_ENUMERATION,
                        location_of(id));
                }
            }
        }
        if (is_float(column.get_kind()) || column.get_kind() == type_kind::COMPTIME_FLOAT) {
            return make_sema_err("Cannot match on floats due to precision; use an if statement "
                                 "with explicit precision handling",
                                 error::TYPE_MISMATCH,
                                 location_of(id));
        }
        if (column.get_data().is<types::struct_t>() || column.get_data().is<types::array>() ||
            column.get_kind() == type_kind::SLICE) {
            return make_sema_err(
                fmt::format("A value of type '{}' can only be matched through its parts",
                            cx_.ctx.type_display_name(column)),
                error::ILLEGAL_MATCH_PATTERN,
                location_of(id));
        }
        if (cx_.callbacks.resolve_leaf) {
            if (auto diag{cx_.callbacks.resolve_leaf(id, column)}) {
                return stdx::err{std::move(*diag)};
            }
        }

        const auto folded{cx_.callbacks.fold(id)};
        if (!folded) {
            if (!top_level) {
                return make_sema_err("A nested pattern must be known at compile time",
                                     error::ILLEGAL_MATCH_PATTERN,
                                     location_of(id));
            }
            return make_node(node_kind::OPAQUE, id, column);
        }

        if (const auto enum_data{column.get_data().as_opt<types::enum_t>()}) {
            const auto en{folded->as_opt<gir::const_enum>()};
            if (en) {
                for (usize i{0}; i < enum_data->ast_enumerations.size(); ++i) {
                    if (enum_variant_name(*enum_data, i) == en->name) {
                        auto out{make_node(node_kind::ENUM_VALUE, id, column)};
                        out->index = i;
                        out->name  = enum_variant_name(*enum_data, i);
                        return out;
                    }
                }
            }
            return make_node(node_kind::OPAQUE, id, column);
        }
        if (const auto b{folded->as_opt<bool>()}; b && column.get_kind() == type_kind::BOOL) {
            auto out{make_node(node_kind::BOOL_VALUE, id, column)};
            out->boolean = *b;
            return out;
        }
        if (is_int_column(column, cx_.target_ptr_bits)) {
            if (const auto v{folded->as_big_int_opt()}) {
                auto out{make_node(node_kind::INT_RANGE, id, column)};
                out->lo = *v;
                out->hi = *v;
                return out;
            }
        }
        return make_node(node_kind::OPAQUE, id, column);
    }

    const elaboration_context& cx_;
    const ast::AST&            tree_;
};

auto collect_captures(const node& n, std::vector<capture_site>& out) -> void {
    for (const auto& site : n.captures) { out.emplace_back(site); }
    for (const auto& sub : n.children) { collect_captures(*sub.pattern, out); }
}

// ---------------------------------------------------------------------- decision trees

struct row {
    usize                    id{0};
    usize                    arm{0};
    bool                     guarded{false};
    std::vector<const node*> cells{};
    std::vector<binding>     bindings{};
};

struct witness_step {
    usize       occ{0};
    std::string label;
};

class tree_builder {
  public:
    tree_builder(gsl::span<const row_input> inputs, type& scrutinee, u32 ptr_bits)
        : ptr_bits_{ptr_bits} {
        tree_.occurrences.emplace_back(occurrence{.value_type = &scrutinee});
        tree_.row_reached.assign(inputs.size(), false);

        std::vector<row> rows;
        rows.reserve(inputs.size());
        for (usize i{0}; i < inputs.size(); ++i) {
            row r{.id = i, .arm = inputs[i].arm, .guarded = inputs[i].guarded};
            add_cell(r, inputs[i].root, 0);
            rows.emplace_back(std::move(r));
        }
        std::vector<usize>        columns{0};
        std::vector<witness_step> path;
        tree_.root = decide(std::move(rows), std::move(columns), path);
    }

    [[nodiscard]] auto take() && -> decision_tree { return std::move(tree_); }

  private:
    static auto add_cell(row& r, const node* cell, usize occ) -> void {
        for (const auto& site : cell->captures) { r.bindings.emplace_back(site.capture, occ); }
        r.cells.emplace_back(cell);
    }

    [[nodiscard]] auto wildcard(type& t) -> const node* {
        wildcards_.emplace_back(make_node(node_kind::WILDCARD, stdx::none, t));
        return wildcards_.back().get();
    }

    auto add_node(decision d) -> usize {
        tree_.nodes.emplace_back(std::move(d));
        return tree_.nodes.size() - 1;
    }

    auto add_occurrence(usize parent, const child& c, step_kind step) -> usize {
        tree_.occurrences.emplace_back(occurrence{
            .parent     = parent,
            .step       = step,
            .index      = c.index,
            .name       = c.name,
            .value_type = &c.value_type,
            .bit_packed = c.bit_packed,
        });
        return tree_.occurrences.size() - 1;
    }

    // Reads a witness back out of the steps that led to a failure
    [[nodiscard]] auto render(usize occ, gsl::span<const witness_step> path) const -> std::string {
        std::string label;
        for (const auto& s : path) {
            if (s.occ == occ) { label = s.label; }
        }

        std::vector<std::pair<usize, std::string>> parts;
        for (usize i{occ + 1}; i < tree_.occurrences.size(); ++i) {
            if (tree_.occurrences[i].parent != occ) { continue; }
            const auto constrained{std::ranges::any_of(path, [&](const witness_step& s) {
                for (stdx::opt_size at{s.occ}; at; at = tree_.occurrences[*at].parent) {
                    if (*at == i) { return true; }
                }
                return false;
            })};
            if (constrained) { parts.emplace_back(i, render(i, path)); }
        }
        if (parts.empty()) { return label.empty() ? "_" : label; }

        const auto first_step{tree_.occurrences[parts.front().first].step};
        if (first_step == step_kind::DEREF) { return parts.front().second; }
        if (first_step == step_kind::PAYLOAD) {
            return fmt::format("{}({})", label, parts.front().second);
        }
        if (first_step == step_kind::FIELD) {
            std::vector<std::string> fields;
            for (const auto& [at, text] : parts) {
                fields.emplace_back(fmt::format(".{} = {}", tree_.occurrences[at].name, text));
            }
            return fmt::format(".{{ {} }}", fmt::join(fields, ", "));
        }
        const auto arr{tree_.occurrences[occ].value_type->get_data().as_opt<types::array>()};
        std::vector<std::string> elements(arr ? arr->len : parts.size(), "_");
        for (const auto& [at, text] : parts) { elements[tree_.occurrences[at].index] = text; }
        return fmt::format(".{{ {} }}", fmt::join(elements, ", "));
    }

    auto fail(gsl::span<const witness_step> path) -> usize {
        if (tree_.missing.size() < 8) {
            auto text{render(0, path)};
            if (std::ranges::any_of(path, [](const witness_step& s) { return s.occ != 0; })) {
                tree_.missing_is_flat = false;
            }
            if (!std::ranges::contains(tree_.missing, text)) {
                tree_.missing.emplace_back(std::move(text));
            }
        }
        return add_node(decision{});
    }

    auto decide(std::vector<row> rows, std::vector<usize> columns, std::vector<witness_step>& path)
        -> usize {
        if (tree_.nodes.size() > MAX_DECISION_NODES) {
            tree_.too_complex = true;
            return add_node(decision{});
        }
        if (rows.empty()) { return fail(path); }

        const auto& first{rows.front()};
        const auto  col{std::ranges::find_if(
            first.cells, [](const node* cell) { return cell->kind != node_kind::WILDCARD; })};
        if (col == first.cells.end()) { return leaf(std::move(rows), std::move(columns), path); }
        const auto col_idx{static_cast<usize>(col - first.cells.begin())};

        const auto holds_opaque{std::ranges::any_of(
            rows, [&](const row& r) { return r.cells[col_idx]->kind == node_kind::OPAQUE; })};
        if (holds_opaque) { return sequence(std::move(rows), std::move(columns), col_idx, path); }

        switch (first.cells[col_idx]->kind) {
        case node_kind::RECORD:
        case node_kind::ARRAY:
        case node_kind::DEREF:  return expand(std::move(rows), std::move(columns), col_idx, path);
        case node_kind::VARIANT:
            return variants(std::move(rows), std::move(columns), col_idx, path);
        case node_kind::ENUM_VALUE:
            return enums(std::move(rows), std::move(columns), col_idx, path);
        case node_kind::BOOL_VALUE:
            return bools(std::move(rows), std::move(columns), col_idx, path);
        case node_kind::INT_RANGE:
            return intervals(std::move(rows), std::move(columns), col_idx, path);
        default: UNREACHABLE("Wildcards and opaque cells were handled above");
        }
    }

    auto leaf(std::vector<row> rows, std::vector<usize> columns, std::vector<witness_step>& path)
        -> usize {
        const auto& first{rows.front()};
        tree_.row_reached[first.id] = true;
        decision out{.kind = decision_kind::LEAF, .arm = first.arm, .bindings = first.bindings};
        if (first.guarded) {
            std::vector<row> rest(std::make_move_iterator(rows.begin() + 1),
                                  std::make_move_iterator(rows.end()));
            out.fallthrough = decide(std::move(rest), std::move(columns), path);
        }
        return add_node(std::move(out));
    }

    // Drops column `at` from a row, splicing `replacement` cells in its place
    static auto replace_cell(row& r, usize at, gsl::span<const node* const> replacement) -> void {
        r.cells.erase(r.cells.begin() + static_cast<std::ptrdiff_t>(at));
        r.cells.insert(r.cells.begin() + static_cast<std::ptrdiff_t>(at),
                       replacement.begin(),
                       replacement.end());
    }

    // A value with one shape needs no test: its parts just become columns of their own
    auto expand(std::vector<row>           rows,
                std::vector<usize>         columns,
                usize                      col,
                std::vector<witness_step>& path) -> usize {
        const auto parent{columns[col]};
        const auto kind{rows.front().cells[col]->kind};
        const auto step{kind == node_kind::RECORD  ? step_kind::FIELD
                        : kind == node_kind::ARRAY ? step_kind::ELEMENT
                                                   : step_kind::DEREF};

        // Every part any row looks at, in layout order
        std::vector<const child*> parts;
        for (const auto& r : rows) {
            for (const auto& c : r.cells[col]->children) {
                const auto known{std::ranges::any_of(
                    parts, [&](const child* p) { return p->index == c.index; })};
                if (!known) { parts.emplace_back(&c); }
            }
        }
        std::ranges::sort(parts, {}, [](const child* c) { return c->index; });

        std::vector<usize> part_occs;
        part_occs.reserve(parts.size());
        for (const auto* part : parts) {
            part_occs.emplace_back(add_occurrence(parent, *part, step));
        }

        for (auto& r : rows) {
            const auto*              cell{r.cells[col]};
            std::vector<const node*> replacement;
            replacement.reserve(parts.size());
            for (usize i{0}; i < parts.size(); ++i) {
                const auto  found{std::ranges::find_if(
                    cell->children, [&](const child& c) { return c.index == parts[i]->index; })};
                const node* sub{found != cell->children.end() ? found->pattern.get()
                                                              : wildcard(parts[i]->value_type)};
                for (const auto& site : sub->captures) {
                    r.bindings.emplace_back(site.capture, part_occs[i]);
                }
                replacement.emplace_back(sub);
            }
            replace_cell(r, col, replacement);
        }
        columns.erase(columns.begin() + static_cast<std::ptrdiff_t>(col));
        columns.insert(
            columns.begin() + static_cast<std::ptrdiff_t>(col), part_occs.begin(), part_occs.end());
        return decide(std::move(rows), std::move(columns), path);
    }

    // Rows left when `col` holds nothing a test could tell apart
    [[nodiscard]] static auto default_rows(const std::vector<row>& rows, usize col)
        -> std::vector<row> {
        std::vector<row> out;
        for (const auto& r : rows) {
            if (r.cells[col]->kind == node_kind::WILDCARD) {
                auto copy{r};
                replace_cell(copy, col, {});
                out.emplace_back(std::move(copy));
            }
        }
        return out;
    }

    [[nodiscard]] static auto without_column(std::vector<usize> columns, usize col)
        -> std::vector<usize> {
        columns.erase(columns.begin() + static_cast<std::ptrdiff_t>(col));
        return columns;
    }

    auto variants(std::vector<row>           rows,
                  std::vector<usize>         columns,
                  usize                      col,
                  std::vector<witness_step>& path) -> usize {
        const auto occ{columns[col]};
        auto&      union_type{*tree_.occurrences[occ].value_type};
        const auto union_data{union_type.get_data().as<types::union_t>()};

        std::vector<usize> listed;
        for (const auto& r : rows) {
            const auto* cell{r.cells[col]};
            if (cell->kind == node_kind::VARIANT && !std::ranges::contains(listed, cell->index)) {
                listed.emplace_back(cell->index);
            }
        }

        decision out{.kind = decision_kind::SWITCH, .occ = occ};
        for (const auto variant : listed) {
            auto&      payload{union_data.type_at(variant)};
            const bool has_payload{payload.get_kind() != type_kind::VOID_};
            const auto payload_occ{[&]() -> stdx::opt_size {
                if (!has_payload) { return stdx::none; }
                const child part{
                    variant, union_field_name(union_data, variant), payload, false, {}};
                return add_occurrence(occ, part, step_kind::PAYLOAD);
            }()};

            std::vector<row> specialized;
            for (const auto& r : rows) {
                const auto* cell{r.cells[col]};
                if (cell->kind == node_kind::VARIANT && cell->index != variant) { continue; }
                auto        copy{r};
                const node* sub{nullptr};
                if (has_payload) {
                    sub = cell->kind == node_kind::VARIANT ? cell->children.front().pattern.get()
                                                           : wildcard(payload);
                    for (const auto& site : sub->captures) {
                        copy.bindings.emplace_back(site.capture, *payload_occ);
                    }
                }
                const std::vector<const node*> cells{has_payload ? std::vector<const node*>{sub}
                                                                 : std::vector<const node*>{}};
                replace_cell(copy, col, cells);
                specialized.emplace_back(std::move(copy));
            }

            auto next_columns{columns};
            next_columns.erase(next_columns.begin() + static_cast<std::ptrdiff_t>(col));
            if (payload_occ) {
                next_columns.insert(next_columns.begin() + static_cast<std::ptrdiff_t>(col),
                                    *payload_occ);
            }
            path.emplace_back(occ, fmt::format(".{}", union_field_name(union_data, variant)));
            const auto target{decide(std::move(specialized), std::move(next_columns), path)};
            path.pop_back();
            out.cases.emplace_back(
                switch_case{.kind = test_kind::TAG, .index = variant, .target = target});
        }

        if (listed.size() < union_data.fields.size()) {
            std::vector<std::string> missing;
            for (usize i{0}; i < union_data.fields.size(); ++i) {
                if (!std::ranges::contains(listed, i)) {
                    missing.emplace_back(fmt::format(".{}", union_field_name(union_data, i)));
                }
            }
            out.otherwise = decide_default(rows, columns, col, occ, missing, path);
        }
        return add_node(std::move(out));
    }

    // What runs for every constructor no row names, with those constructors as its witnesses
    auto decide_default(const std::vector<row>&      rows,
                        const std::vector<usize>&    columns,
                        usize                        col,
                        usize                        occ,
                        gsl::span<const std::string> missing,
                        std::vector<witness_step>&   path) -> usize {
        auto fallback{default_rows(rows, col)};
        if (!fallback.empty()) {
            return decide(std::move(fallback), without_column(columns, col), path);
        }
        usize target{0};
        for (const auto& label : missing) {
            path.emplace_back(occ, label);
            target = fail(path);
            path.pop_back();
        }
        return target;
    }

    auto enums(std::vector<row>           rows,
               std::vector<usize>         columns,
               usize                      col,
               std::vector<witness_step>& path) -> usize {
        const auto occ{columns[col]};
        auto&      enum_type{*tree_.occurrences[occ].value_type};
        const auto enum_data{enum_type.get_data().as<types::enum_t>()};

        std::vector<const node*> listed;
        for (const auto& r : rows) {
            const auto* cell{r.cells[col]};
            if (cell->kind == node_kind::ENUM_VALUE &&
                std::ranges::none_of(listed,
                                     [&](const node* l) { return l->index == cell->index; })) {
                listed.emplace_back(cell);
            }
        }

        decision out{.kind = decision_kind::SWITCH, .occ = occ};
        for (const auto* variant : listed) {
            std::vector<row> specialized;
            for (const auto& r : rows) {
                const auto* cell{r.cells[col]};
                if (cell->kind == node_kind::ENUM_VALUE && cell->index != variant->index) {
                    continue;
                }
                auto copy{r};
                replace_cell(copy, col, {});
                specialized.emplace_back(std::move(copy));
            }
            path.emplace_back(occ, fmt::format(".{}", variant->name));
            const auto target{decide(std::move(specialized), without_column(columns, col), path)};
            path.pop_back();
            out.cases.emplace_back(switch_case{.kind   = test_kind::ENUM,
                                               .index  = variant->index,
                                               .source = variant->source,
                                               .target = target});
        }

        if (listed.size() < enum_data.ast_enumerations.size() || enum_data.non_exhaustive) {
            std::vector<std::string> missing;
            for (usize i{0}; i < enum_data.ast_enumerations.size(); ++i) {
                const auto named{
                    std::ranges::any_of(listed, [&](const node* l) { return l->index == i; })};
                if (!named) {
                    missing.emplace_back(fmt::format(".{}", enum_variant_name(enum_data, i)));
                }
            }
            if (missing.empty()) { missing.emplace_back("_"); }
            out.otherwise = decide_default(rows, columns, col, occ, missing, path);
        }
        return add_node(std::move(out));
    }

    auto bools(std::vector<row>           rows,
               std::vector<usize>         columns,
               usize                      col,
               std::vector<witness_step>& path) -> usize {
        const auto               occ{columns[col]};
        decision                 out{.kind = decision_kind::SWITCH, .occ = occ};
        std::vector<std::string> missing;
        for (const bool value : {true, false}) {
            const auto named{std::ranges::any_of(rows, [&](const row& r) {
                const auto* cell{r.cells[col]};
                return cell->kind == node_kind::BOOL_VALUE && cell->boolean == value;
            })};
            if (!named) {
                missing.emplace_back(value ? "true" : "false");
                continue;
            }
            std::vector<row> specialized;
            for (const auto& r : rows) {
                const auto* cell{r.cells[col]};
                if (cell->kind == node_kind::BOOL_VALUE && cell->boolean != value) { continue; }
                auto copy{r};
                replace_cell(copy, col, {});
                specialized.emplace_back(std::move(copy));
            }
            path.emplace_back(occ, value ? "true" : "false");
            const auto target{decide(std::move(specialized), without_column(columns, col), path)};
            path.pop_back();
            out.cases.emplace_back(
                switch_case{.kind = test_kind::BOOL, .boolean = value, .target = target});
        }
        if (!missing.empty()) {
            out.otherwise = decide_default(rows, columns, col, occ, missing, path);
        }
        return add_node(std::move(out));
    }

    auto intervals(std::vector<row>           rows,
                   std::vector<usize>         columns,
                   usize                      col,
                   std::vector<witness_step>& path) -> usize {
        const auto occ{columns[col]};
        const auto one{big_int{i128{1}}};

        // Cut the number line wherever a range starts or ends, so each piece is all-or-nothing
        std::vector<big_int> cuts;
        for (const auto& r : rows) {
            const auto* cell{r.cells[col]};
            if (cell->kind == node_kind::INT_RANGE) {
                cuts.emplace_back(cell->lo);
                cuts.emplace_back(cell->hi + one);
            }
        }
        std::ranges::sort(cuts);
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

        struct piece {
            big_int            lo;
            big_int            hi;
            std::vector<usize> covering;
        };
        std::vector<piece> pieces;
        for (usize i{0}; i + 1 < cuts.size(); ++i) {
            const auto         lo{cuts[i]};
            const auto         hi{cuts[i + 1] - one};
            std::vector<usize> covering;
            for (usize k{0}; k < rows.size(); ++k) {
                const auto* cell{rows[k].cells[col]};
                if (cell->kind == node_kind::INT_RANGE && cell->lo <= lo && hi <= cell->hi) {
                    covering.emplace_back(k);
                }
            }
            if (covering.empty()) { continue; }
            // Neighbors that select the same rows are one test
            if (!pieces.empty() && pieces.back().covering == covering &&
                pieces.back().hi + one == lo) {
                pieces.back().hi = hi;
            } else {
                pieces.emplace_back(lo, hi, std::move(covering));
            }
        }

        decision out{.kind = decision_kind::SWITCH, .occ = occ};
        for (const auto& p : pieces) {
            std::vector<row> specialized;
            for (usize k{0}; k < rows.size(); ++k) {
                if (rows[k].cells[col]->kind == node_kind::INT_RANGE &&
                    !std::ranges::contains(p.covering, k)) {
                    continue;
                }
                auto copy{rows[k]};
                replace_cell(copy, col, {});
                specialized.emplace_back(std::move(copy));
            }
            path.emplace_back(occ, interval_label(p.lo, p.hi));
            const auto target{decide(std::move(specialized), without_column(columns, col), path)};
            path.pop_back();
            out.cases.emplace_back(
                switch_case{.kind = test_kind::INTERVAL, .lo = p.lo, .hi = p.hi, .target = target});
        }

        // Whatever the pieces leave of the type's range goes to the wildcard rows
        std::vector<std::string> gaps;
        const auto               bounds{bounds_of(*tree_.occurrences[occ].value_type, ptr_bits_)};
        if (!bounds) {
            gaps.emplace_back("_");
        } else {
            auto next{bounds->lo};
            for (const auto& p : pieces) {
                if (next < p.lo) { gaps.emplace_back(interval_label(next, p.lo - one)); }
                next = p.hi + one;
            }
            if (next <= bounds->hi) { gaps.emplace_back(interval_label(next, bounds->hi)); }
        }
        if (!gaps.empty()) { out.otherwise = decide_default(rows, columns, col, occ, gaps, path); }
        return add_node(std::move(out));
    }

    [[nodiscard]] static auto interval_label(const big_int& lo, const big_int& hi) -> std::string {
        return lo == hi ? lo.to_string() : fmt::format("{}..={}", lo.to_string(), hi.to_string());
    }

    // An opaque pattern can overlap anything, so rows are tried strictly one at a time
    auto sequence(std::vector<row>           rows,
                  std::vector<usize>         columns,
                  usize                      col,
                  std::vector<witness_step>& path) -> usize {
        const auto  occ{columns[col]};
        const auto* cell{rows.front().cells[col]};
        ASSERT(cell->source, "An opaque test comes from a written pattern");

        decision out{.kind = decision_kind::SEQUENCE, .occ = occ, .test = cell->source};

        std::vector<row> rest(rows.begin() + 1, rows.end());
        out.on_false = decide(std::move(rest), columns, path);

        auto matched{std::move(rows)};
        matched.front().cells[col] = wildcard(*cell->value_type);
        out.on_true                = decide(std::move(matched), std::move(columns), path);
        return add_node(std::move(out));
    }

    u32                   ptr_bits_;
    decision_tree         tree_;
    std::vector<node_ptr> wildcards_;
};

} // namespace

auto elaborate(const elaboration_context& cx,
               ast::node_id               pattern,
               type&                      scrutinee_type,
               bool                       place_is_const) -> stdx::result<node_ptr, diagnostic> {
    return elaborator{cx}.run(pattern, scrutinee_type, place_is_const, true);
}

auto rows_of(const ast::match_expr& match, gsl::span<const std::vector<node_ptr>> arm_patterns)
    -> std::vector<row_input> {
    std::vector<row_input> rows;
    const auto             add_arm{[&](usize arm) {
        for (const auto& alternative : arm_patterns[arm]) {
            rows.emplace_back(arm, alternative.get(), match.arms[arm].guard.has_value());
        }
    }};
    for (usize arm{0}; arm < match.arms.size(); ++arm) {
        if (match.catch_all_idx != arm) { add_arm(arm); }
    }
    if (match.catch_all_idx) { add_arm(*match.catch_all_idx); }
    return rows;
}

auto attach_arm_capture(const ast::capture_t& capture, bool place_is_const, node& root) -> void {
    if (!capture.payload.is<ast::identifier_expr>()) { return; }
    auto* target{&root};
    while (target->kind == node_kind::DEREF) { target = target->children.front().pattern.get(); }
    if (target->kind == node_kind::VARIANT && !target->children.empty()) {
        auto& payload{target->children.front()};
        payload.pattern->captures.emplace_back(capture, payload.value_type, place_is_const, false);
        return;
    }
    // A void variant's payload has no storage, so the capture only needs its type
    if (target->kind == node_kind::VARIANT) {
        const auto union_data{target->value_type->get_data().as<types::union_t>()};
        root.captures.emplace_back(
            capture, union_data.type_at(target->index), place_is_const, false);
        return;
    }
    root.captures.emplace_back(capture, *root.value_type, place_is_const, false);
}

auto binds_any_name(const node& root) noexcept -> bool {
    if (!root.captures.empty()) { return true; }
    return std::ranges::any_of(root.children,
                               [](const child& c) { return binds_any_name(*c.pattern); });
}

auto captures_of(const node& root) -> std::vector<capture_site> {
    std::vector<capture_site> out;
    collect_captures(root, out);
    return out;
}

auto compile(gsl::span<const row_input> rows, type& scrutinee_type, u32 target_ptr_bits)
    -> decision_tree {
    PROFILE_FUNCTION();
    return tree_builder{rows, scrutinee_type, target_ptr_bits}.take();
}

} // namespace ghoti::sema::pattern
