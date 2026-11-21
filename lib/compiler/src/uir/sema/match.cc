#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/expression.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/pattern.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/big_int.hh"
#include "support/int128.hh"

namespace ghoti::uir::sema {

namespace pattern = gs::pattern;

namespace {

[[nodiscard]] auto make_node(pattern::node_kind kind, gs::type& type) -> pattern::node_ptr {
    auto out{stdx::make_nullable_box<pattern::node>(
        pattern::node{.source = stdx::none, .value_type = &type})};
    out->kind = kind;
    return out;
}

// The smallest and largest values of an integer type, for a range pattern with an open end
[[nodiscard]] auto int_bounds(const gs::type& t, u32 ptr_bits) -> std::pair<big_int, big_int> {
    const u32  bits{t.get_kind() == gs::type_kind::INT ? gs::int_width(t) : ptr_bits};
    const bool is_signed{gs::is_signed_integer(t)};
    return {big_int::min_of(bits, is_signed), big_int::max_of(bits, is_signed)};
}

// Whether the comptime `value` of `type` is one `node` matches
[[nodiscard]] auto matches(const ip::pool& pool, const pattern::node& node, ip::index value)
    -> bool {
    const auto& v{pool.value_at(value)};
    switch (node.kind) {
    case pattern::node_kind::WILDCARD:   return true;
    case pattern::node_kind::BOOL_VALUE: {
        const auto b{v.data.as_opt<bool>()};
        return b && *b == node.boolean;
    }
    case pattern::node_kind::ENUM_VALUE:
    case pattern::node_kind::INT_RANGE:  {
        const auto i{int_of(v)};
        return i && node.lo <= *i && *i <= node.hi;
    }
    case pattern::node_kind::VARIANT: {
        const auto un{v.data.as_opt<ip::union_value>()};
        return un && un->active_field == node.name;
    }
    default: return false;
    }
}

} // namespace

auto analyzer::capture_name(const ast::capture_t& capture) const -> std::string_view {
    return u_.source->ast.get_as<ast::identifier_expr>(*capture.payload).name;
}

auto analyzer::match_arms(inst_id inst) -> std::vector<match_arm> {
    auto rec{record(inst)};
    rec.skip_ref().skip_u32().skip_index();
    std::vector<match_arm> arms;
    const auto             count{rec.count()};
    for (u32 i{0}; i < count; ++i) {
        const auto patterns{rec.refs_field()};
        rec.skip_body();
        const auto capture{rec.ref_field()};
        const auto guard{rec.body_field()};
        const auto body{rec.body_field()};
        arms.emplace_back<match_arm>(
            {.patterns = patterns, .capture = capture, .guard = guard, .body = body});
    }

    return arms;
}

auto analyzer::pattern_value(gsl::span<const inst_id> body, gs::type& type, const source& src)
    -> result<ip::index> {
    const auto value{[&] -> result<operand> {
        const auto scope{comptime_scope()};
        const auto out{TRY(analyze_body(body))};
        if (!out.yielded) { return comp_.unsupported(u_, src, "a pattern that yields nothing"); }
        return *out.yielded;
    }()};
    if (!value && value.error() == stop::RUNTIME) {
        return comp_.unsupported(u_, src, "a pattern compared at runtime");
    }

    const auto known{TRY(value)};
    if (type.get_kind() == gs::type_kind::ARRAY &&
        type_of(known).get_kind() == gs::type_kind::ARRAY) {
        return comp_.unsupported(u_, src, "an array pattern");
    }

    const auto typed{TRY(coerce_to(known, type, src, {.what = use_site::kind::STORE}))};
    return require_comptime(typed, src);
}

auto analyzer::pattern_node(inst_id pat, gs::type& type) -> result<pattern::node_ptr> {
    const auto& src{src_of(pat)};
    auto&       pool{comp_.constants()};
    switch (mod().tag_of(pat)) {
    case tag::pat_wildcard: return make_node(pattern::node_kind::WILDCARD, type);
    case tag::pat_value:    {
        auto       rec{record(pat)};
        const auto body{rec.body_field()};

        // `.name` against a tagged union names one of its fields
        if (const auto ut{type.get_data().as_opt<gs::types::union_t>()}) {
            if (ut->is_untagged || body.empty() ||
                mod().tag_of(body.front()) != tag::implicit_access) {
                return comp_.unsupported(u_, src, "this union pattern");
            }

            const auto name{mod().string(mod().data_of(body.front()).str)};
            const auto index{TRY(field_index(type, name, src))};
            auto&      payload{ut->type_at(index)};
            auto       out{make_node(pattern::node_kind::VARIANT, type)};
            out->index = index;
            out->name  = ut->field_names[index];
            if (payload.get_kind() != gs::type_kind::VOID_) {
                out->children.emplace_back(index,
                                           out->name,
                                           payload,
                                           false,
                                           make_node(pattern::node_kind::WILDCARD, payload));
            }

            return out;
        }

        const auto  value{TRY(pattern_value(body, type, src))};
        const auto& data{pool.value_at(value).data};
        if (type.get_kind() == gs::type_kind::BOOL) {
            auto out{make_node(pattern::node_kind::BOOL_VALUE, type)};
            out->boolean = *data.as_opt<bool>();
            return out;
        }

        const auto number{int_of(pool.value_at(value))};
        if (!number) { return comp_.unsupported(u_, src, "a pattern of this type"); }
        if (type.get_kind() == gs::type_kind::ENUM) {
            const auto n{comp_.nominal_of(type)};
            if (!n) {
                return comp_.unsupported(u_, src, "a variant of an enum UIR didn't declare");
            }

            const auto at{std::ranges::find(n->enum_values, static_cast<i64>(*number->to_i128()))};
            auto       out{make_node(pattern::node_kind::ENUM_VALUE, type)};
            out->index = static_cast<usize>(at - n->enum_values.begin());
            out->name  = type.get_data().as<gs::types::enum_t>().variant_names[out->index];
            out->lo    = *number;
            out->hi    = *number;
            return out;
        }

        if (!gs::is_integer(type.get_kind())) {
            return comp_.unsupported(u_, src, "a pattern of this type");
        }

        auto out{make_node(pattern::node_kind::INT_RANGE, type)};
        out->lo = *number;
        out->hi = *number;
        return out;
    }
    case tag::pat_range: {
        if (!gs::is_integer(type.get_kind())) {
            return comp_.unsupported(u_, src, "a range pattern of this type");
        }

        auto       rec{record(pat)};
        const auto lo_body{rec.body_field()};
        const auto hi_body{rec.body_field()};
        const bool inclusive{(rec.u32_field() & flags::INCLUSIVE) != 0};
        const auto [min, max]{int_bounds(type, comp_.ptr_bits())};
        auto out{make_node(pattern::node_kind::INT_RANGE, type)};
        out->lo = min;
        out->hi = max;
        if (!lo_body.empty()) {
            out->lo = *int_of(pool.value_at(TRY(pattern_value(lo_body, type, src))));
        }

        if (!hi_body.empty()) {
            const auto hi{*int_of(pool.value_at(TRY(pattern_value(hi_body, type, src))))};
            out->hi = inclusive ? hi : hi - big_int{i128{1}};
        }

        return out;
    }
    default: break;
    }

    return comp_.unsupported(u_, src, fmt::format("the `{}` pattern", tag_name(mod().tag_of(pat))));
}

auto analyzer::analyze_match(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  operand_ref{*rec.ref_field()};
    const auto  match_flags{rec.u32_field()};
    const auto  catch_all{rec.index_field()};
    if ((match_flags & flags::MATCH_BINDING) != 0) {
        return comp_.unsupported(u_, src, "a destructuring");
    }

    const bool comptime_match{(match_flags & flags::MATCH_COMPTIME) != 0};
    const auto arms{match_arms(inst)};

    const auto scrutinee{TRY([&] -> result<operand> {
        if (!comptime_match || is_comptime()) { return resolve_value(operand_ref, src); }
        const auto scope{comptime_scope()};
        return at_comptime_boundary(reevaluate(operand_ref, src));
    }())};
    auto&      type{type_of(scrutinee)};
    if (type.is_poison()) { return stdx::err{stop::POISON}; }
    if (type.get_kind() == gs::type_kind::REFERENCE || type.get_kind() == gs::type_kind::POINTER) {
        return comp_.unsupported(u_, src, "a match through a reference");
    }

    if (!gs::is_value_type(type.get_kind()) || type.get_kind() == gs::type_kind::TYPE ||
        gs::is_comptime_numeric(type.get_kind())) {
        return comp_.unsupported(u_, src, "a match on a type or untyped value");
    }

    TRY(comp_.require_fields(u_, src, type));

    // The arms' patterns, with each arm's `|v|` attached to them
    const auto& ast_match{u_.source->ast.get_as<ast::match_expr>(*src.as_opt<ast::node_id>())};
    std::vector<std::vector<pattern::node_ptr>> patterns(arms.size());
    for (usize a{0}; a < arms.size(); ++a) {
        if (!arms[a].guard.empty()) { return comp_.unsupported(u_, src, "a match guard"); }
        for (const auto p : arms[a].patterns) {
            auto node{TRY(pattern_node(*as_inst(p), type))};
            if (const auto& capture{ast_match.arms[a].capture}) {
                pattern::attach_arm_capture(*capture, false, *node);
            }

            patterns[a].emplace_back(std::move(node));
        }
    }

    // The catch-all arm is tried last, whatever its place
    std::vector<pattern::row_input> rows;
    const auto                      add_arm{[&](usize a) {
        for (const auto& alternative : patterns[a]) {
            rows.emplace_back<pattern::row_input>(
                {.arm = a, .root = alternative.get(), .guarded = false});
        }
    }};
    for (usize a{0}; a < arms.size(); ++a) {
        if (catch_all != a) { add_arm(a); }
    }

    if (catch_all) { add_arm(*catch_all); }

    if (comptime_match || is_comptime()) { return run_match(arms, rows, scrutinee, src); }

    const auto tree{pattern::compile(rows, type, comp_.ptr_bits())};
    if (!tree.missing.empty()) {
        return comp_.report(u_,
                            src,
                            gs::error::MISSING_FIELD,
                            fmt::format("Match expression is non-exhaustive; missing: {}",
                                        fmt::join(tree.missing, ", ")));
    }

    if (const auto folded{TRY(fold_match(arms, rows, operand_ref, src))}) { return *folded; }
    return emit_match(arms, rows, tree, operand_ref, scrutinee, src);
}

auto analyzer::fold_match(const std::vector<match_arm>&              arms,
                          const std::vector<gs::pattern::row_input>& rows,
                          ref                                        operand_ref,
                          const source& src) -> result<stdx::option<operand>> {
    // A failure only means the match runs at runtime, so nothing it records is kept
    const auto before{comp_.speculate()};
    const auto folded{[&] -> result<operand> {
        const auto scope{comptime_scope()};
        const auto scrutinee{TRY(reevaluate(operand_ref, src))};
        return run_match(arms, rows, scrutinee, src);
    }()};
    const bool unsupported{!folded && folded.error() == stop::UNSUPPORTED};
    comp_.abandon(before, unsupported);

    // An arm that leaves the match, like a `return`, stays for runtime
    const bool jumped{jump_.has_value()};
    jump_.reset();
    if (unsupported) { return stdx::err{stop::UNSUPPORTED}; }
    if (jumped || !folded) { return stdx::option<operand>{}; }

    // Only a match whose value is a number or `bool` folds
    const auto index{comptime_value(*folded)};
    if (!index) { return stdx::option<operand>{}; }
    const auto kind{type_of(*folded).get_kind()};
    if (kind != gs::type_kind::BOOL && !gs::is_integer(kind) && !gs::is_float(kind) &&
        !gs::is_comptime_numeric(kind)) {
        return stdx::option<operand>{};
    }

    return stdx::option<operand>{*folded};
}

auto analyzer::run_match(const std::vector<match_arm>&          arms,
                         const std::vector<pattern::row_input>& rows,
                         const operand&                         scrutinee,
                         const source&                          src) -> result<operand> {
    const auto value{TRY(require_comptime(scrutinee, src))};
    auto&      pool{comp_.constants()};
    for (const auto& row : rows) {
        if (!matches(pool, *row.root, value)) { continue; }
        const auto& arm{arms[row.arm]};
        if (arm.capture) {
            const auto un{pool.value_at(value).data.as_opt<ip::union_value>()};
            const bool payload{un && row.root->kind == pattern::node_kind::VARIANT};
            operand    bound{value};
            if (payload) {
                bound = un->payload.empty() ? void_value() : operand{un->payload.front()};
            }

            values_.insert_or_assign(*as_inst(*arm.capture), bound);
        }

        const auto out{TRY(analyze_body(arm.body))};
        if (jump_ || (!is_comptime() && terminated())) { return void_value(); }
        return out.yielded.value_or(void_value());
    }

    return comp_.report(
        u_, src, gs::error::COMPTIME_EVALUATION_FAILED, "No match arm matched the value");
}

auto analyzer::scrutinee_address(ref            operand_ref,
                                 const operand& scrutinee,
                                 bool           aggregate,
                                 const source&  src) -> result<gir::value> {
    // A named aggregate is taken apart where it lives, anything else from a copy
    const auto inst{as_inst(operand_ref)};
    const auto names_storage{[&] {
        if (!inst) { return false; }
        switch (mod().tag_of(*inst)) {
        case tag::param:
        case tag::let_value:
        case tag::bind:
        case tag::decl_val:  return true;
        default:             return false;
        }
    }()};
    if (aggregate) {
        if (names_storage) { return TRY(address_of(operand_ref, src)).address; }
        if (inst && mod().tag_of(*inst) == tag::load) {
            return TRY(address_of(mod().data_of(*inst).un.operand, src)).address;
        }

        return comp_.unsupported(u_, src, "a match on an aggregate with no name");
    }

    const auto value{TRY(as_runtime(scrutinee, src))};
    return spill(value, *value.type, false).address;
}

auto analyzer::occurrence_address(const match_lowering& m, usize occ) -> gir::value {
    const auto& at{m.tree.occurrences[occ]};
    if (!at.parent) { return m.root; }
    const auto parent{occurrence_address(m, *at.parent)};
    auto&      field_type{*at.value_type};
    switch (at.step) {
    case pattern::step_kind::PAYLOAD:
        return gir::value{builder_.emit_get_element_ptr(
                              parent, {usize_constant(UNION_PAYLOAD_FIELD)}, field_type),
                          field_type};
    default:
        return gir::value{
            builder_.emit_get_element_ptr(parent, {usize_constant(at.index)}, field_type),
            field_type};
    }
}

auto analyzer::emit_decision(const match_lowering& m, usize node) -> result<void> {
    auto&       bool_type{builtin(gs::type_kind::BOOL)};
    auto&       i32_type{comp_.ctx().get_int(32, true)};
    auto&       pool{comp_.constants()};
    const auto& d{m.tree.nodes[node]};
    switch (d.kind) {
    case pattern::decision_kind::FAIL: builder_.emit_unreachable(); return {};
    case pattern::decision_kind::LEAF: {
        for (const auto& bind : d.bindings) {
            const auto name{capture_name(bind.capture)};
            const auto slot{std::ranges::find_if(
                m.slots[d.arm], [&](const capture_slot& s) { return s.name == name; })};
            if (slot == m.slots[d.arm].end() || slot->is_void) { continue; }
            const auto address{occurrence_address(m, bind.occ)};
            const auto loaded{
                builder_.emit_load(gir::value{address.data, *slot->type}, *slot->type)};
            builder_.emit_store(slot->address, gir::value{loaded, *slot->type}).is_initializer =
                true;
        }

        emit_goto(m.bodies[d.arm]);
        return {};
    }
    case pattern::decision_kind::SEQUENCE:
        return comp_.unsupported(u_, m.src, "a pattern compared at runtime");
    case pattern::decision_kind::SWITCH: break;
    }

    const auto address{occurrence_address(m, d.occ)};
    auto&      subject_type{*m.tree.occurrences[d.occ].value_type};
    const bool is_tag{d.cases.front().kind == pattern::test_kind::TAG};
    const auto subject{[&] {
        if (!is_tag) {
            return gir::value{
                builder_.emit_load(gir::value{address.data, subject_type}, subject_type),
                subject_type};
        }

        const auto tag_at{
            builder_.emit_get_element_ptr(address, {usize_constant(UNION_TAG_FIELD)}, i32_type)};
        return gir::value{builder_.emit_load(gir::value{tag_at, i32_type}, i32_type), i32_type};
    }()};

    for (usize i{0}; i < d.cases.size(); ++i) {
        const auto& test{d.cases[i]};

        // With no fallback, the last case is the only thing left
        if (i + 1 == d.cases.size() && !d.otherwise) { return emit_decision(m, test.target); }
        const auto matched{[&] {
            const auto compare{[&](gir::value bound) {
                return gir::value{
                    builder_.emit_binary(gir::instruction_kind::EQ, subject, bound, bool_type),
                    bool_type};
            }};
            switch (test.kind) {
            case pattern::test_kind::TAG:
                return compare(
                    constant(pool.intern(static_cast<i64>(test.index), i32_type), i32_type));
            case pattern::test_kind::BOOL:
                return compare(constant(pool.intern(test.boolean, bool_type), bool_type));
            case pattern::test_kind::ENUM: {
                const auto n{comp_.nominal_of(subject_type)};
                return compare(
                    constant(pool.intern(n->enum_values[test.index], subject_type), subject_type));
            }
            case pattern::test_kind::INTERVAL: break;
            }

            const auto bound{[&](const big_int& v) {
                return constant(intern_int(pool, v, subject_type), subject_type);
            }};
            if (test.lo == test.hi) { return compare(bound(test.lo)); }
            const auto ge{builder_.emit_binary(
                gir::instruction_kind::GE, subject, bound(test.lo), bool_type)};
            const auto le{builder_.emit_binary(
                gir::instruction_kind::LE, subject, bound(test.hi), bool_type)};
            return gir::value{builder_.emit_binary(gir::instruction_kind::AND,
                                                   gir::value{ge, bool_type},
                                                   gir::value{le, bool_type},
                                                   bool_type),
                              bool_type};
        }()};
        const auto on_match{new_segment()};
        const auto on_miss{new_segment()};
        builder_.emit_cond_goto(matched, on_match, on_miss);
        switch_to(on_match);
        TRY(emit_decision(m, test.target));
        switch_to(on_miss);
    }

    return emit_decision(m, *d.otherwise);
}

auto analyzer::emit_match(const std::vector<match_arm>&          arms,
                          const std::vector<pattern::row_input>& rows,
                          const pattern::decision_tree&          tree,
                          ref                                    operand_ref,
                          const operand&                         scrutinee,
                          const source&                          src) -> result<operand> {
    TRY(require_runtime(src));
    auto&      type{type_of(scrutinee)};
    const auto merge{new_segment()};
    const bool aggregate{
        type.get_data().is<gs::types::union_t>() || type.get_data().is<gs::types::struct_t>() ||
        type.get_data().is<gs::types::array>() || type.get_kind() == gs::type_kind::SLICE};
    match_lowering m{.tree   = tree,
                     .root   = TRY(scrutinee_address(operand_ref, scrutinee, aggregate, src)),
                     .bodies = {},
                     .slots  = std::vector<std::vector<capture_slot>>(arms.size()),
                     .src    = src};
    for (usize a{0}; a < arms.size(); ++a) { m.bodies.emplace_back(new_segment()); }

    // One slot per captured name per arm
    for (const auto& row : rows) {
        for (const auto& site : pattern::captures_of(*row.root)) {
            auto&      slots{m.slots[row.arm]};
            const auto name{capture_name(site.capture)};
            if (std::ranges::any_of(slots, [&](const capture_slot& s) { return s.name == name; })) {
                continue;
            }

            if (site.capture.modifier.is_ref() || site.capture.modifier.is_ptr()) {
                return comp_.unsupported(u_, src, "a capture by reference");
            }

            auto&      cap_type{site.value_type};
            const bool is_void{cap_type.get_kind() == gs::type_kind::VOID_};
            const auto slot{is_void ? gir::value{}
                                    : gir::value{builder_.emit_alloca(cap_type), cap_type}};
            slots.emplace_back<capture_slot>(
                {.name = name, .type = &cap_type, .address = slot, .is_void = is_void});
        }
    }

    TRY(emit_decision(m, tree.root));

    std::vector<bool> reached(arms.size(), false);
    for (usize i{0}; i < rows.size(); ++i) {
        if (tree.row_reached[i]) { reached[rows[i].arm] = true; }
    }

    std::vector<incoming> arrivals;
    for (usize a{0}; a < arms.size(); ++a) {
        if (!reached[a]) { continue; }
        switch_to(m.bodies[a]);
        if (const auto capture{arms[a].capture}) {
            if (m.slots[a].empty()) { return comp_.unsupported(u_, src, "this match capture"); }
            const auto& slot{m.slots[a].front()};
            values_.insert_or_assign(
                *as_inst(*capture),
                slot.is_void ? void_value()
                             : operand{slot_value{.address = slot.address, .type = slot.type}});
        }

        const auto out{TRY(analyze_body(arms[a].body))};
        if (terminated()) { continue; }
        if (out.yielded) {
            arrivals.emplace_back<incoming>(
                {.seg = builder_.get_segment()->get_id(), .value = *out.yielded, .src = src});
        }

        emit_goto(merge);
    }

    return finish_merge(arrivals, merge, src);
}

} // namespace ghoti::uir::sema
