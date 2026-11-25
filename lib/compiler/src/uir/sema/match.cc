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

// Whether any arm captures by reference or pointer, aliasing the scrutinee
[[nodiscard]] auto holds_alias(const std::vector<pattern::row_input>& rows) -> bool {
    return std::ranges::any_of(rows, [](const pattern::row_input& row) {
        return std::ranges::any_of(pattern::captures_of(*row.root), [](const auto& site) {
            return site.capture.modifier.is_ref() || site.capture.modifier.is_ptr();
        });
    });
}

// Whether a capture may write through what it aliases
[[nodiscard]] auto holds_mutable_alias(const std::vector<pattern::row_input>& rows) -> bool {
    return std::ranges::any_of(rows, [](const pattern::row_input& row) {
        return std::ranges::any_of(pattern::captures_of(*row.root), [](const auto& site) {
            const auto modifier{site.capture.modifier};
            return modifier.is_mutable_ref() || modifier.is_mutable_ptr() || modifier.is_poly();
        });
    });
}

// Everything a bit-packed field's sub-pattern binds lives in bit-packed storage
auto mark_bit_packed(pattern::node& n) -> void {
    for (auto& site : n.captures) { site.place_is_bit_packed = true; }
    for (auto& sub : n.children) { mark_bit_packed(*sub.pattern); }
}

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
        const auto pattern_body{rec.body_field()};
        const auto capture{rec.ref_field()};
        const auto guard{rec.body_field()};
        const auto body{rec.body_field()};
        arms.emplace_back<match_arm>({.patterns     = patterns,
                                      .pattern_body = pattern_body,
                                      .capture      = capture,
                                      .guard        = guard,
                                      .body         = body});
    }

    return arms;
}

auto analyzer::pattern_operand(gsl::span<const inst_id> body, const source& src)
    -> result<operand> {
    const auto value{[&] -> result<operand> {
        const auto scope{comptime_scope()};
        const auto out{TRY(analyze_body(body))};
        if (!out.yielded) { return comp_.unsupported(u_, src, "a pattern that yields nothing"); }
        return *out.yielded;
    }()};
    if (!value && value.error() == stop::RUNTIME) {
        return comp_.unsupported(u_, src, "a pattern compared at runtime");
    }

    return value;
}

auto analyzer::pattern_value(gsl::span<const inst_id> body, gs::type& type, const source& src)
    -> result<ip::index> {
    const auto known{TRY(pattern_operand(body, src))};
    if (type.get_kind() == gs::type_kind::ARRAY &&
        type_of(known).get_kind() == gs::type_kind::ARRAY) {
        return comp_.unsupported(u_, src, "an array pattern");
    }

    const auto typed{TRY(coerce_to(known, type, src, {.what = use_site::kind::STORE}))};
    if (const auto literal{typed.as_opt<enum_literal>()}) { return literal_value(*literal); }
    return require_comptime(typed, src);
}

auto analyzer::pattern_capture(inst_id pat) const -> const ast::capture_t& {
    const auto  id{*src_of(pat).as_opt<ast::node_id>()};
    const auto& tree{u_.source->ast};
    if (const auto bound{tree.get_as_opt<ast::bound_pattern_expr>(id)}) { return bound->capture; }
    return tree.get_as<ast::capture_pattern_expr>(id).capture;
}

auto analyzer::pattern_node(inst_id pat, gs::type& type, bool place_is_const, bool top_level)
    -> result<pattern::node_ptr> {
    const auto& src{src_of(pat)};
    auto&       pool{comp_.constants()};
    const auto  t{mod().tag_of(pat)};

    // A `&T` is looked through by everything but a capture of the reference itself
    if (const auto referenced{type.get_data().as_opt<gs::types::reference>()};
        referenced && t != tag::pat_capture && t != tag::pat_wildcard) {
        if (t == tag::pat_bound) {
            const auto& d{mod().data_of(pat)};
            auto inner{TRY(pattern_node(*as_inst(d.bin.lhs), type, place_is_const, top_level))};
            if (const auto cap{as_inst(d.bin.rhs)}) {
                inner->captures.emplace_back(pattern_capture(*cap), type, place_is_const, false);
            }

            return inner;
        }

        auto& referent{referenced->underlying};
        auto  sub{TRY(pattern_node(pat, referent, type.is_constant(), top_level))};
        auto  out{make_node(pattern::node_kind::DEREF, type)};
        out->children.emplace_back(0, "", referent, false, std::move(sub));
        return out;
    }

    switch (t) {
    case tag::pat_wildcard: return make_node(pattern::node_kind::WILDCARD, type);
    case tag::pat_capture:  {
        auto rec{record(pat)};
        rec.skip_str().skip_u32().skip_u32();
        const auto sub{rec.ref_field()};
        auto       out{sub ? TRY(pattern_node(*as_inst(*sub), type, place_is_const, top_level))
                           : make_node(pattern::node_kind::WILDCARD, type)};
        out->captures.emplace_back(pattern_capture(pat), type, place_is_const, false);
        return out;
    }
    case tag::pat_bound: {
        const auto& d{mod().data_of(pat)};
        const auto  inner_pat{*as_inst(d.bin.lhs)};

        // `.ok |x|` reads like `.ok => |x|`, but would bind the whole union
        if (const auto ut{type.get_data().as_opt<gs::types::union_t>()};
            ut && mod().tag_of(inner_pat) == tag::pat_value) {
            auto       rec{record(inner_pat)};
            const auto body{rec.body_field()};
            if (!body.empty() && mod().tag_of(body.front()) == tag::implicit_access) {
                const auto name{mod().string(mod().data_of(body.front()).str)};
                const auto at{std::ranges::find(ut->field_names, name)};
                if (at != ut->field_names.end() &&
                    ut->type_at(static_cast<usize>(at - ut->field_names.begin())).get_kind() !=
                        gs::type_kind::VOID_) {
                    return comp_.report(
                        u_,
                        src_of(inner_pat),
                        gs::error::ILLEGAL_MATCH_PATTERN,
                        fmt::format("'.{0} |x|' would bind the whole union; write '.{0}(|x|)' "
                                    "for the payload or '.{0}(_) |x|' for the union",
                                    name));
                }
            }
        }

        auto out{TRY(pattern_node(inner_pat, type, place_is_const, top_level))};
        if (const auto cap{as_inst(d.bin.rhs)}) {
            out->captures.emplace_back(pattern_capture(*cap), type, place_is_const, false);
        }

        return out;
    }
    case tag::pat_init:  return aggregate_pattern(pat, type, place_is_const);
    case tag::pat_call:  return variant_pattern(pat, type, place_is_const);
    case tag::pat_value: {
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

        // A string against bytes tests them one by one
        const bool bytes{type.get_kind() == gs::type_kind::SLICE ||
                         type.get_kind() == gs::type_kind::ARRAY};
        if (bytes && !body.empty() && mod().tag_of(body.front()) == tag::str) {
            const auto text{TRY(pattern_operand(body, src))};
            if (const auto known{comptime_value(text)}) {
                return string_pattern(pat, *known, type);
            }
        }

        if (gs::is_float(type.get_kind()) || type.get_kind() == gs::type_kind::COMPTIME_FLOAT) {
            return comp_.report(u_,
                                src,
                                gs::error::TYPE_MISMATCH,
                                "Cannot match on floats due to precision; use an if statement "
                                "with explicit precision handling");
        }

        if (type.get_data().is<gs::types::struct_t>() || bytes) {
            return comp_.report(
                u_,
                src,
                gs::error::ILLEGAL_MATCH_PATTERN,
                fmt::format("A value of type '{}' can only be matched through its parts",
                            comp_.ctx().type_display_name(type)));
        }

        const auto known{TRY(known_pattern(body, type, src))};
        if (!known) { return opaque_pattern(pat, type, top_level); }
        if (pool.is_type(*known)) { return opaque_pattern(pat, type, true); }
        const auto  value{*known};
        const auto& data{pool.value_at(value).data};
        if (type.get_kind() == gs::type_kind::BOOL) {
            auto out{make_node(pattern::node_kind::BOOL_VALUE, type)};
            out->boolean = *data.as_opt<bool>();
            return out;
        }

        const auto number{int_of(pool.value_at(value))};
        if (!number) { return opaque_pattern(pat, type, true); }
        if (type.get_kind() == gs::type_kind::ENUM) {
            const auto n{comp_.nominal_of(type)};
            if (!n) {
                return comp_.unsupported(u_, src, "a variant of an enum UIR didn't declare");
            }

            const auto at{std::ranges::find(n->enum_values, static_cast<i64>(*number->to_i128()))};
            if (at == n->enum_values.end()) { return opaque_pattern(pat, type, true); }
            auto out{make_node(pattern::node_kind::ENUM_VALUE, type)};
            out->index = static_cast<usize>(at - n->enum_values.begin());
            out->name  = type.get_data().as<gs::types::enum_t>().variant_names[out->index];
            out->lo    = *number;
            out->hi    = *number;
            return out;
        }

        if (!gs::is_integer(type.get_kind())) { return opaque_pattern(pat, type, true); }

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
        const auto lo{lo_body.empty() ? stdx::option<ip::index>{}
                                      : TRY(known_pattern(lo_body, type, src))};
        const auto hi{hi_body.empty() ? stdx::option<ip::index>{}
                                      : TRY(known_pattern(hi_body, type, src))};

        // An end only known at runtime is compared there
        if ((!lo_body.empty() && !lo) || (!hi_body.empty() && !hi)) {
            return opaque_pattern(pat, type, top_level);
        }

        auto out{make_node(pattern::node_kind::INT_RANGE, type)};
        out->lo = lo ? *int_of(pool.value_at(*lo)) : min;
        out->hi = max;
        if (hi) {
            const auto last{*int_of(pool.value_at(*hi))};
            out->hi = inclusive ? last : last - big_int{i128{1}};
        }

        if (out->hi < out->lo) {
            return comp_.report(u_,
                                src,
                                gs::error::ILLEGAL_MATCH_PATTERN,
                                "Range pattern is empty; its lower bound exceeds its upper bound");
        }

        return out;
    }
    default: break;
    }

    return comp_.unsupported(u_, src, fmt::format("the `{}` pattern", tag_name(mod().tag_of(pat))));
}

auto analyzer::known_pattern(gsl::span<const inst_id> body, gs::type& type, const source& src)
    -> result<stdx::option<ip::index>> {
    // A value only known at runtime leaves nothing of its attempt behind
    const auto before{comp_.speculate()};
    const auto value{[&] -> result<operand> {
        const auto scope{comptime_scope()};
        const auto out{TRY(analyze_body(body))};
        if (!out.yielded) { return comp_.unsupported(u_, src, "a pattern that yields nothing"); }
        return *out.yielded;
    }()};
    if (!value && value.error() == stop::RUNTIME) {
        comp_.abandon(before, false);
        return stdx::option<ip::index>{};
    }

    const auto known{TRY(value)};
    if (const auto index{comptime_value(known)}; index && comp_.constants().is_type(*index)) {
        return stdx::option<ip::index>{*index};
    }

    const auto typed{TRY(coerce_to(known, type, src, {.what = use_site::kind::STORE}))};
    return stdx::option<ip::index>{TRY(require_comptime(typed, src))};
}

auto analyzer::opaque_pattern(inst_id pat, gs::type& type, bool top_level)
    -> result<pattern::node_ptr> {
    const auto& src{src_of(pat)};
    if (!top_level) {
        return comp_.report(u_,
                            src,
                            gs::error::ILLEGAL_MATCH_PATTERN,
                            "A nested pattern must be known at compile time");
    }

    const auto id{*src.as_opt<ast::node_id>()};
    opaque_patterns_.insert_or_assign(id.get_index(), pat);
    auto out{make_node(pattern::node_kind::OPAQUE, type)};
    out->source = id;
    return out;
}

auto analyzer::string_pattern(inst_id pat, ip::index text, gs::type& type)
    -> result<pattern::node_ptr> {
    const auto& src{src_of(pat)};
    const auto  bytes{comp_.constants().value_at(text).data.as_opt<std::string>()};
    auto&       byte{type.get_kind() == gs::type_kind::SLICE
                         ? type.get_data().as<gs::types::slice>().underlying
                         : type.get_data().as<gs::types::array>().underlying};
    const auto  byte_info{gs::as_integer(byte)};
    if (!bytes || !byte_info || byte_info->bits != 8 || byte_info->is_signed) {
        return comp_.report(u_,
                            src,
                            gs::error::ILLEGAL_MATCH_PATTERN,
                            fmt::format("A string can't match a value of type '{}'",
                                        comp_.ctx().type_display_name(type)));
    }

    auto each{make_node(pattern::node_kind::ARRAY, type)};
    for (usize i{0}; i < bytes->size(); ++i) {
        auto element{make_node(pattern::node_kind::INT_RANGE, byte)};
        element->lo = big_int{i128{static_cast<u8>((*bytes)[i])}};
        element->hi = element->lo;
        each->children.emplace_back(i, "", byte, false, std::move(element));
    }

    if (const auto arr{type.get_data().as_opt<gs::types::array>()}) {
        if (arr->len != bytes->size()) {
            return comp_.report(u_,
                                src,
                                gs::error::ILLEGAL_MATCH_PATTERN,
                                fmt::format("A string of {} bytes can't match a '{}'",
                                            bytes->size(),
                                            comp_.ctx().type_display_name(type)));
        }

        return each;
    }

    auto& usize_type{builtin(gs::type_kind::USIZE)};
    auto  len{make_node(pattern::node_kind::INT_RANGE, usize_type)};
    len->lo = big_int{u128{bytes->size()}};
    len->hi = len->lo;
    auto out{make_node(pattern::node_kind::RECORD, type)};
    out->children.emplace_back(SLICE_LEN_FIELD, "len", usize_type, false, std::move(len));
    if (!bytes->empty()) {
        out->children.emplace_back(pattern::SLICE_BYTES_FIELD_INDEX,
                                   pattern::SLICE_BYTES_FIELD_NAME,
                                   type,
                                   false,
                                   std::move(each));
    }

    return out;
}

auto analyzer::opaque_test(const gir::value& subject, inst_id pat) -> result<gir::value> {
    const auto& src{src_of(pat)};
    auto&       bool_type{builtin(gs::type_kind::BOOL)};
    auto&       subject_type{*subject.type};
    const auto  side{[&](gsl::span<const inst_id> body) -> result<gir::value> {
        const auto out{TRY(analyze_body(body))};
        if (!out.yielded) { return comp_.unsupported(u_, src, "a pattern that yields nothing"); }
        const auto typed{
            TRY(coerce_to(*out.yielded, subject_type, src, {.what = use_site::kind::STORE}))};
        return as_runtime(typed, src);
    }};
    const auto  compare{[&](gir::instruction_kind kind, const gir::value& bound) {
        return gir::value{builder_.emit_binary(kind, subject, bound, bool_type), bool_type};
    }};

    auto rec{record(pat)};
    if (mod().tag_of(pat) != tag::pat_range) {
        return compare(gir::instruction_kind::EQ, TRY(side(rec.body_field())));
    }

    const auto               lo_body{rec.body_field()};
    const auto               hi_body{rec.body_field()};
    const bool               inclusive{(rec.u32_field() & flags::INCLUSIVE) != 0};
    stdx::option<gir::value> matched;
    const auto               combine{[&](const gir::value& next) {
        matched = matched ? gir::value{builder_.emit_binary(
                                           gir::instruction_kind::AND, *matched, next, bool_type),
                                       bool_type}
                          : next;
    }};
    if (!lo_body.empty()) { combine(compare(gir::instruction_kind::GE, TRY(side(lo_body)))); }
    if (!hi_body.empty()) {
        combine(compare(inclusive ? gir::instruction_kind::LE : gir::instruction_kind::LT,
                        TRY(side(hi_body))));
    }

    return *matched;
}

auto analyzer::known_opaque(const pattern::node& node, ip::index value, const source& src)
    -> result<bool> {
    auto&      pool{comp_.constants()};
    const auto pat{opaque_patterns_.at(node.source->get_index())};
    const auto side{[&](gsl::span<const inst_id> body) -> result<ip::index> {
        const auto known{TRY(known_pattern(body, *node.value_type, src))};
        if (!known) { return stdx::err{stop::RUNTIME}; }
        return *known;
    }};

    auto rec{record(pat)};
    if (mod().tag_of(pat) != tag::pat_range) {
        const auto bound{TRY(side(rec.body_field()))};
        if (bound == value) { return true; }
        if (pool.is_type(bound) || pool.is_type(value)) {
            return pool.is_type(bound) && pool.is_type(value) &&
                   &pool.type_at(bound) == &pool.type_at(value);
        }

        const auto lhs{int_of(pool.value_at(bound))};
        const auto rhs{int_of(pool.value_at(value))};
        if (!lhs || !rhs) { return comp_.unsupported(u_, src, "this pattern at compile time"); }
        return *lhs == *rhs;
    }

    const auto lo_body{rec.body_field()};
    const auto hi_body{rec.body_field()};
    const bool inclusive{(rec.u32_field() & flags::INCLUSIVE) != 0};
    const auto at{int_of(pool.value_at(value))};
    if (!at) { return comp_.unsupported(u_, src, "this pattern at compile time"); }
    if (!lo_body.empty() && *at < *int_of(pool.value_at(TRY(side(lo_body))))) { return false; }
    if (!hi_body.empty()) {
        const auto hi{*int_of(pool.value_at(TRY(side(hi_body))))};
        if (inclusive ? hi < *at : !(*at < hi)) { return false; }
    }

    return true;
}

auto analyzer::aggregate_pattern(inst_id pat, gs::type& type, bool place_is_const)
    -> result<pattern::node_ptr> {
    const auto& src{src_of(pat)};
    auto        rec{record(pat)};
    rec.skip_body();
    std::vector<std::pair<std::string_view, inst_id>> entries;
    const auto                                        count{rec.count()};
    for (u32 i{0}; i < count; ++i) {
        const auto name{rec.str_field()};
        entries.emplace_back(name, *as_inst(*rec.ref_field()));
    }

    const auto mismatch{[&](const source& at, gs::error code, std::string message) {
        return comp_.report(u_, at, code, std::move(message));
    }};
    const auto named{
        std::ranges::count_if(entries, [](const auto& e) { return !e.first.empty(); })};
    if (const auto arr{type.get_data().as_opt<gs::types::array>()}) {
        if (named != 0) {
            return mismatch(src,
                            gs::error::ILLEGAL_MATCH_PATTERN,
                            "An array pattern lists its elements by position");
        }

        if (entries.size() != arr->len) {
            return mismatch(src,
                            gs::error::ILLEGAL_MATCH_PATTERN,
                            fmt::format("Pattern has {} elements, but the array has {}",
                                        entries.size(),
                                        arr->len));
        }

        auto  out{make_node(pattern::node_kind::ARRAY, type)};
        auto& element{arr->underlying};
        for (usize i{0}; i < entries.size(); ++i) {
            auto sub{TRY(pattern_node(entries[i].second, element, place_is_const, false))};
            out->children.emplace_back(i, "", element, false, std::move(sub));
        }

        return out;
    }

    const auto st{type.get_data().as_opt<gs::types::struct_t>()};
    const auto slice{type.get_data().as_opt<gs::types::slice>()};
    if (!st && !slice) {
        return mismatch(src,
                        gs::error::ILLEGAL_MATCH_PATTERN,
                        fmt::format("Cannot destructure a value of type '{}'",
                                    comp_.ctx().type_display_name(type)));
    }

    if (named == 0 && !entries.empty()) {
        return mismatch(src,
                        gs::error::ILLEGAL_MATCH_PATTERN,
                        "A struct pattern names its fields: `.{ .x = ... }`");
    }

    TRY(comp_.require_fields(u_, src, type));
    auto                                           out{make_node(pattern::node_kind::RECORD, type)};
    ankerl::unordered_dense::set<std::string_view> seen;
    for (const auto& [name, sub_pat] : entries) {
        const auto& at{src_of(sub_pat)};
        if (name.empty()) {
            return mismatch(at,
                            gs::error::ILLEGAL_MATCH_PATTERN,
                            "A struct pattern names its fields: `.{ .x = ... }`");
        }

        if (!seen.insert(name).second) {
            return mismatch(at,
                            gs::error::DUPLICATE_FIELD,
                            fmt::format("Field '{}' appears twice in this pattern", name));
        }

        if (slice) {
            if (name != "len" && name != "ptr") {
                return mismatch(
                    at, gs::error::UNKNOWN_FIELD, fmt::format("A slice has no field '{}'", name));
            }

            const bool is_len{name == "len"};
            auto&      field_type{
                is_len ? builtin(gs::type_kind::USIZE)
                       : comp_.ctx().get_pointer(gs::types::mut::CONSTANT, slice->underlying)};
            auto sub{TRY(pattern_node(sub_pat, field_type, place_is_const, false))};
            out->children.emplace_back(is_len ? SLICE_LEN_FIELD : SLICE_PTR_FIELD,
                                       name,
                                       field_type,
                                       false,
                                       std::move(sub));
            continue;
        }

        const auto index{index_of(st->field_names, name)};
        if (!index) {
            return mismatch(
                at,
                gs::error::UNKNOWN_FIELD,
                fmt::format("'{}' has no field '{}'", comp_.ctx().type_display_name(type), name));
        }

        if (&st->enclosing != u_.source.get() && !st->is_field_public(*index)) {
            return mismatch(at,
                            gs::error::ILLEGAL_PRIVATE_ACCESS,
                            fmt::format("Field '{}' of '{}' is private",
                                        name,
                                        comp_.ctx().type_display_name(type)));
        }

        auto&      field_type{st->type_at(*index)};
        auto       sub{TRY(pattern_node(sub_pat, field_type, place_is_const, false))};
        const bool bit_packed{st->is_bit_packed()};
        if (bit_packed) { mark_bit_packed(*sub); }
        out->children.emplace_back(*index, name, field_type, bit_packed, std::move(sub));
    }

    return out;
}

auto analyzer::variant_pattern(inst_id pat, gs::type& type, bool place_is_const)
    -> result<pattern::node_ptr> {
    const auto& src{src_of(pat)};
    const auto  ut{type.get_data().as_opt<gs::types::union_t>()};
    if (!ut) { return comp_.unsupported(u_, src, "a variant pattern of this type"); }
    if (ut->is_untagged) {
        return comp_.report(u_,
                            src,
                            gs::error::TYPE_MISMATCH,
                            "Cannot match on an untagged union; it has no runtime tag");
    }

    // `.name(p)`, or `Union.name(p)`, whose qualifier only names the union
    auto       rec{record(pat)};
    const auto callee{rec.body_field()};
    const auto args{rec.refs_field()};
    const auto name{[&] -> stdx::option<std::string_view> {
        stdx::option<std::string_view> named;
        for (const auto inst : callee) {
            if (mod().tag_of(inst) == tag::implicit_access) {
                named = mod().string(mod().data_of(inst).str);
            } else if (mod().tag_of(inst) == tag::field_val) {
                auto field{record(inst)};
                field.skip_ref();
                named = field.str_field();
            }
        }

        return named;
    }()};
    if (!name) {
        return comp_.report(u_,
                            src,
                            gs::error::ILLEGAL_MATCH_PATTERN,
                            "Match arm may only name a variant of the union in this context");
    }

    TRY(comp_.require_fields(u_, src, type));
    const auto index{index_of(ut->field_names, *name)};
    if (!index) {
        return comp_.report(
            u_,
            src,
            gs::error::UNKNOWN_FIELD,
            fmt::format("Match expression contains unknown union field: {}", *name));
    }

    auto& payload{ut->type_at(*index)};
    if (payload.get_kind() == gs::type_kind::VOID_) {
        return comp_.report(u_,
                            src,
                            gs::error::ILLEGAL_MATCH_PATTERN,
                            fmt::format("Variant '{}' has no payload to match", *name));
    }

    if (args.size() != 1) {
        return comp_.report(
            u_,
            src,
            gs::error::ILLEGAL_MATCH_PATTERN,
            fmt::format("Variant '{}' takes exactly one pattern for its payload", *name));
    }

    auto out{make_node(pattern::node_kind::VARIANT, type)};
    out->index = *index;
    out->name  = ut->field_names[*index];
    auto sub{TRY(pattern_node(*as_inst(args.front()), payload, place_is_const, false))};
    out->children.emplace_back(*index, out->name, payload, false, std::move(sub));
    return out;
}

auto analyzer::analyze_match(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  operand_ref{*rec.ref_field()};
    const auto  match_flags{rec.u32_field()};
    const auto  catch_all{rec.index_field()};
    // A destructuring's check, which binds nothing itself
    const bool binding{(match_flags & flags::MATCH_BINDING) != 0};

    const bool comptime_match{(match_flags & flags::MATCH_COMPTIME) != 0};
    const auto arms{match_arms(inst)};

    auto  scrutinee{TRY([&] -> result<operand> {
        if (!comptime_match || is_comptime()) { return resolve_value(operand_ref, src); }
        const auto scope{comptime_scope()};
        return at_comptime_boundary(reevaluate(operand_ref, src));
    }())};
    auto& scrutinee_type{type_of(scrutinee)};
    if (scrutinee_type.is_poison()) { return stdx::err{stop::POISON}; }
    if (scrutinee_type.get_kind() == gs::type_kind::POINTER) {
        return comp_.unsupported(u_, src, "a match through a pointer");
    }

    // A reference is matched on what it refers to, a compile-time one by the value it holds
    const auto given{scrutinee};
    if (const auto referent{comptime_referent(scrutinee)}) { scrutinee = operand{*referent}; }

    auto&      matched_type{type_of(scrutinee)};
    const bool through_ref{matched_type.get_kind() == gs::type_kind::REFERENCE};
    if (through_ref && (comptime_match || is_comptime())) {
        return comp_.unsupported(u_, src, "a match through a reference at compile time");
    }

    auto& type{through_ref ? *gs::pointee_of(matched_type) : matched_type};

    // A type or an untyped number is only matched at compile time, by each pattern's value
    const bool by_value{type.get_kind() == gs::type_kind::TYPE ||
                        type.get_kind() == gs::type_kind::ENUM_LITERAL ||
                        gs::is_comptime_numeric(type.get_kind())};
    if (by_value && (comptime_match || is_comptime() || comptime_value(scrutinee) ||
                     scrutinee.is<enum_literal>())) {
        if (type.get_kind() == gs::type_kind::TYPE) { TRY(check_type_match(catch_all, src)); }
        return match_by_value(arms, catch_all, scrutinee, type, src);
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
                {.arm = a, .root = alternative.get(), .guarded = !arms[a].guard.empty()});
        }
    }};
    for (usize a{0}; a < arms.size(); ++a) {
        if (catch_all != a) { add_arm(a); }
    }

    if (catch_all) { add_arm(*catch_all); }

    if (comptime_match || is_comptime()) {
        const auto root{holds_alias(rows) ? TRY(comptime_scrutinee(operand_ref, given, src))
                                          : stdx::none};
        return run_match(arms, rows, scrutinee, src, binding, root);
    }

    const auto tree{pattern::compile(rows, type, comp_.ptr_bits())};
    if (!tree.missing.empty() && binding) {
        return comp_.report(u_,
                            src_of(*as_inst(arms.front().patterns.front())),
                            gs::error::ILLEGAL_MATCH_PATTERN,
                            fmt::format("This pattern can fail, so it needs an `else` or a "
                                        "`match`; missing: {}",
                                        fmt::join(tree.missing, ", ")));
    }

    if (!tree.missing.empty()) {
        return comp_.report(u_,
                            src,
                            gs::error::MISSING_FIELD,
                            fmt::format("Match expression is non-exhaustive; missing: {}",
                                        fmt::join(tree.missing, ", ")));
    }

    // A capture that may write through the scrutinee keeps the match at runtime
    if (!through_ref && !holds_mutable_alias(rows)) {
        if (const auto folded{TRY(fold_match(arms, rows, operand_ref, src))}) { return *folded; }
    }

    return emit_match(arms,
                      rows,
                      tree,
                      operand_ref,
                      scrutinee,
                      type,
                      src,
                      binding ? catch_all : stdx::option<u32>{});
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
        const auto root{holds_alias(rows) ? TRY(comptime_scrutinee(operand_ref, scrutinee, src))
                                          : stdx::none};
        return run_match(arms, rows, scrutinee, src, false, root);
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

auto analyzer::check_type_match(stdx::option<u32> catch_all, const source& src) -> result<void> {
    const auto& ast_match{u_.source->ast.get_as<ast::match_expr>(*src.as_opt<ast::node_id>())};
    for (const auto& arm : ast_match.arms) {
        if (!arm.guard) { continue; }
        return comp_.report(u_,
                            source{*arm.guard},
                            gs::error::ILLEGAL_MATCH_PATTERN,
                            "A 'match' on a type can't take a guard");
    }

    if (!catch_all) {
        return comp_.report(u_,
                            src,
                            gs::error::ILLEGAL_MATCH_PATTERN,
                            "A 'match' on a type requires a catch-all '_' arm");
    }

    for (const auto& arm : ast_match.arms) {
        if (!arm.capture) { continue; }
        return comp_.report(u_,
                            source{*arm.capture->payload},
                            gs::error::ILLEGAL_MATCH_PATTERN,
                            "A 'match' on a type cannot bind a capture");
    }

    return {};
}

auto analyzer::match_by_value(const std::vector<match_arm>& arms,
                              stdx::option<u32>             catch_all,
                              const operand&                scrutinee,
                              gs::type&                     type,
                              const source&                 src) -> result<operand> {
    const auto literal{scrutinee.as_opt<enum_literal>()};
    const auto value{literal ? literal_value(*literal) : TRY(require_comptime(scrutinee, src))};
    auto&      pool{comp_.constants()};
    const auto same{[&](ip::index pattern) {
        if (pattern == value) { return true; }
        if (pool.is_type(pattern) && pool.is_type(value)) {
            return &pool.type_at(pattern) == &pool.type_at(value);
        }

        const auto lhs{int_of(pool.value_at(pattern))};
        const auto rhs{int_of(pool.value_at(value))};
        return lhs && rhs && *lhs == *rhs;
    }};

    // The catch-all arm is tried last, whatever its place
    std::vector<usize> order;
    for (usize a{0}; a < arms.size(); ++a) {
        if (catch_all != a) { order.emplace_back(a); }
    }

    if (catch_all) { order.emplace_back(*catch_all); }
    for (const auto a : order) {
        const auto& arm{arms[a]};
        if (!arm.guard.empty()) { return comp_.unsupported(u_, src, "a match guard"); }
        bool hit{false};
        for (const auto p : arm.patterns) {
            const auto pat{*as_inst(p)};
            if (mod().tag_of(pat) == tag::pat_wildcard) {
                hit = true;
                break;
            }

            if (mod().tag_of(pat) != tag::pat_value) {
                return comp_.unsupported(
                    u_, src_of(pat), "this pattern of a type or untyped value");
            }

            auto       rec{record(pat)};
            const auto body{rec.body_field()};

            // A type's arms name types, which are compared as they are
            if (type.get_kind() == gs::type_kind::TYPE) {
                const auto known{comptime_value(TRY(pattern_operand(body, src_of(pat))))};
                if (!known || !pool.is_type(*known)) {
                    return comp_.report(
                        u_,
                        src_of(pat),
                        gs::error::ILLEGAL_MATCH_PATTERN,
                        "A 'match' on a type expects every arm pattern to be a type");
                }

                if (same(*known)) {
                    hit = true;
                    break;
                }

                continue;
            }

            if (same(TRY(pattern_value(body, type, src_of(pat))))) {
                hit = true;
                break;
            }
        }

        if (!hit) { continue; }
        if (arm.capture) { values_.insert_or_assign(*as_inst(*arm.capture), operand{value}); }
        const auto out{TRY(analyze_body(arm.body))};
        if (jump_ || (!is_comptime() && terminated())) { return void_value(); }
        return out.yielded.value_or(void_value());
    }

    return comp_.report(
        u_, src, gs::error::COMPTIME_EVALUATION_FAILED, "No match arm matched the value");
}

template <typename Lookup> void analyzer::bind_arm_names(const match_arm& arm, Lookup&& lookup) {
    const auto arm_capture{arm.capture.and_then([](ref r) { return as_inst(r); })};
    for (const auto inst : arm.pattern_body) {
        if (mod().tag_of(inst) != tag::bind || inst == arm_capture) { continue; }
        auto       rec{record(inst)};
        const auto name{rec.str_field()};
        if (auto value{lookup(name)}) { values_.insert_or_assign(inst, std::move(*value)); }
    }
}

auto analyzer::known_match(const pattern::node&                                 node,
                           ip::index                                            value,
                           std::vector<std::pair<std::string_view, ip::index>>& bound,
                           const source&                                        src,
                           const stdx::option<comptime_alias>& at) -> result<bool> {
    auto& pool{comp_.constants()};

    // A `&v` or `^v` capture binds the address of where its value lives
    const auto bind_captures{[&] -> result<void> {
        for (const auto& site : node.captures) {
            if (!site.capture.modifier.is_ref() && !site.capture.modifier.is_ptr()) {
                bound.emplace_back(capture_name(site.capture), value);
                continue;
            }

            if (!at) {
                return comp_.unsupported(
                    u_, src, "a capture by reference of a value with no compile-time storage");
            }

            auto& alias{*TRY(capture_type(site, at->access))};
            bound.emplace_back(capture_name(site.capture), memory_address(at->place, alias));
        }

        return {};
    }};
    const auto child_at{[&](usize index, gs::type& type) -> stdx::option<comptime_alias> {
        if (!at) { return stdx::none; }
        auto inner{*at};
        inner.place.path.emplace_back(comptime_step{static_cast<u32>(index), &type});
        return inner;
    }};

    // A type is only taken whole, by a wildcard or a capture
    if (pool.is_type(value)) {
        if (node.kind == pattern::node_kind::OPAQUE && !TRY(known_opaque(node, value, src))) {
            return false;
        }

        if (node.kind != pattern::node_kind::WILDCARD && node.kind != pattern::node_kind::OPAQUE) {
            return comp_.unsupported(u_, src, "a pattern on a type at compile time");
        }

        TRY(bind_captures());
        return true;
    }

    const auto& data{pool.value_at(value).data};
    const auto  child_values{[&] -> result<std::vector<stdx::option<ip::index>>> {
        switch (node.kind) {
        case pattern::node_kind::VARIANT: {
            const auto un{data.as_opt<ip::union_value>()};
            if (!un || un->payload.empty()) { return std::vector<stdx::option<ip::index>>{}; }
            return std::vector<stdx::option<ip::index>>{un->payload.front()};
        }
        case pattern::node_kind::RECORD:
        case pattern::node_kind::ARRAY:  {
            // A string's length, and its bytes one by one
            if (const auto text{data.as_opt<std::string>()}) {
                const auto                           bytes{*text};
                auto&                                usize_type{builtin(gs::type_kind::USIZE)};
                auto&                                byte{comp_.ctx().get_int(8, false)};
                std::vector<stdx::option<ip::index>> out;
                for (const auto& c : node.children) {
                    if (node.kind == pattern::node_kind::ARRAY) {
                        out.emplace_back(c.index < bytes.size()
                                             ? stdx::option<ip::index>{pool.intern(
                                                   u64{static_cast<u8>(bytes[c.index])}, byte)}
                                             : stdx::none);
                    } else if (c.index == SLICE_LEN_FIELD) {
                        out.emplace_back(pool.intern(u64{bytes.size()}, usize_type));
                    } else if (c.index == pattern::SLICE_BYTES_FIELD_INDEX) {
                        out.emplace_back(value);
                    } else {
                        return comp_.unsupported(u_, src, "this pattern at compile time");
                    }
                }
                return out;
            }

            const auto agg{data.as_opt<ip::aggregate_value>()};
            if (!agg || node.value_type->get_kind() == gs::type_kind::SLICE) {
                return comp_.unsupported(u_, src, "this pattern at compile time");
            }

            std::vector<stdx::option<ip::index>> out;
            for (const auto& c : node.children) {
                out.emplace_back(c.index < agg->elements.size() ? agg->elements[c.index]
                                                                : stdx::none);
            }
            return out;
        }
        case pattern::node_kind::DEREF:
            return comp_.unsupported(u_, src, "this pattern at compile time");
        default: return std::vector<stdx::option<ip::index>>{};
        }
    }};

    if (!node.children.empty() || node.kind == pattern::node_kind::RECORD ||
        node.kind == pattern::node_kind::ARRAY) {
        if (node.kind == pattern::node_kind::VARIANT && !matches(pool, node, value)) {
            return false;
        }

        const auto values{TRY(child_values())};
        for (usize i{0}; i < node.children.size(); ++i) {
            const auto child{i < values.size() ? values[i] : stdx::none};
            if (!child) { return comp_.unsupported(u_, src, "an undefined part of a pattern"); }
            const auto& c{node.children[i]};
            const auto  inner{node.kind == pattern::node_kind::VARIANT
                                  ? child_at(node.index, *c.pattern->value_type)
                              : data.is<std::string>() ? stdx::none
                                                       : child_at(c.index, *c.pattern->value_type)};
            if (!TRY(known_match(*c.pattern, *child, bound, src, inner))) { return false; }
        }
    } else if (node.kind == pattern::node_kind::OPAQUE) {
        if (!TRY(known_opaque(node, value, src))) { return false; }
    } else if (!matches(pool, node, value)) {
        return false;
    }

    TRY(bind_captures());
    return true;
}

auto analyzer::run_match(const std::vector<match_arm>&          arms,
                         const std::vector<pattern::row_input>& rows,
                         const operand&                         scrutinee,
                         const source&                          src,
                         bool                                   binding,
                         const stdx::option<comptime_alias>&    root) -> result<operand> {
    const auto value{TRY(require_comptime(scrutinee, src))};
    auto&      pool{comp_.constants()};
    for (const auto& row : rows) {
        std::vector<std::pair<std::string_view, ip::index>> bound_names;
        if (!TRY(known_match(*row.root, value, bound_names, src, root))) { continue; }
        const auto& arm{arms[row.arm]};
        bind_arm_names(arm, [&](std::string_view name) -> stdx::option<operand> {
            const auto found{std::ranges::find_if(
                bound_names, [&](const auto& entry) { return entry.first == name; })};
            if (found == bound_names.end()) { return stdx::none; }
            return operand{found->second};
        });

        // A guard that fails moves on to the next row
        if (!arm.guard.empty()) {
            const auto out{TRY(analyze_body(arm.guard))};
            if (!out.yielded) { return comp_.unsupported(u_, src, "a guard that yields nothing"); }
            const auto passed{TRY(coerce_to(
                *out.yielded, builtin(gs::type_kind::BOOL), src, {.what = use_site::kind::STORE}))};
            const auto known{TRY(require_comptime(passed, src))};
            if (!*pool.value_at(known).data.as_opt<bool>()) { continue; }
        }

        if (arm.capture) {
            const auto un{pool.value_at(value).data.as_opt<ip::union_value>()};
            const bool payload{un && row.root->kind == pattern::node_kind::VARIANT};
            operand    bound{value};
            if (payload) {
                bound = un->payload.empty() ? void_value() : operand{un->payload.front()};
            }

            // A `&v` or `^v` capture is the address its pattern bound under its name
            record_reader capture{record(*as_inst(*arm.capture))};
            const auto    name{capture.str_field()};
            DISCARD(capture.u32_field());
            const ast::type_modifier modifier{u64{capture.u32_field()}};
            const auto               alias{std::ranges::find_if(
                bound_names, [&](const auto& entry) { return entry.first == name; })};
            if ((modifier.is_ref() || modifier.is_ptr()) && alias != bound_names.end()) {
                bound = operand{alias->second};
            }
            values_.insert_or_assign(*as_inst(*arm.capture), bound);
        }

        const auto out{TRY(analyze_body(arm.body))};
        if (jump_ || (!is_comptime() && terminated())) { return void_value(); }
        return out.yielded.value_or(void_value());
    }

    return comp_.report(u_,
                        src,
                        gs::error::COMPTIME_EVALUATION_FAILED,
                        binding ? "This value doesn't fit the pattern it's being taken apart by"
                                : "No match arm matched the value");
}

auto analyzer::comptime_scrutinee(ref operand_ref, const operand& scrutinee, const source& src)
    -> result<stdx::option<comptime_alias>> {
    // A reference is taken apart in the memory it refers to
    auto& type{type_of(scrutinee)};
    if (type.get_kind() == gs::type_kind::REFERENCE) {
        const auto memory{memory_place(scrutinee)};
        if (!memory) { return stdx::none; }
        const bool is_poly{type.is_poly() || TRY(names_poly_param(operand_ref, src))};
        return comptime_alias{
            .place  = *memory,
            .access = {.is_const = type.is_constant(), .is_addressable = true, .is_poly = is_poly}};
    }

    // A named one is taken apart where it lives
    const auto inst{as_inst(operand_ref)};
    const bool reads{inst && mod().tag_of(*inst) == tag::load};
    const auto held{TRY(resolve(reads ? mod().data_of(*inst).un.operand : operand_ref, src))};
    if (const auto p{held.as_opt<comptime_place>()}) {
        // A `let mut` is read through a load, a `let` array of `mut` elements is held itself
        return comptime_alias{.place  = *p,
                              .access = {.is_const       = !reads && type_of(held).is_constant(),
                                         .is_addressable = true,
                                         .is_poly        = false}};
    }

    // Anything else is taken apart in a copy, which only read-only captures may alias
    const auto value{comptime_value(scrutinee)};
    if (!value) { return stdx::none; }
    return comptime_alias{.place  = comp_.new_alloc({.type                = type,
                                                     .value               = *value,
                                                     .frame               = frame_,
                                                     .runtime_depth       = runtime_depth_,
                                                     .keeps_comptime_type = false}),
                          .access = {.is_const = true, .is_addressable = false, .is_poly = false}};
}

auto analyzer::names_poly_param(ref r, const source& src) -> result<bool> {
    const auto inst{as_inst(r)};
    if (!inst || mod().tag_of(*inst) != tag::param) { return false; }
    auto rec{record(*inst)};
    rec.skip_str();
    const auto type_body{rec.body_field()};
    if (type_body.empty()) { return false; }
    return TRY(eval_type(type_body, src))->is_poly();
}

auto analyzer::scrutinee_address(ref            operand_ref,
                                 const operand& scrutinee,
                                 gs::type&      type,
                                 bool           aggregate,
                                 bool           aliased,
                                 const source&  src) -> result<match_root> {
    // A reference is taken apart where it points
    auto& scrutinee_type{type_of(scrutinee)};
    if (scrutinee_type.get_kind() == gs::type_kind::REFERENCE) {
        const auto ref_value{TRY(as_runtime(scrutinee, src))};
        return match_root{.address = gir::value{ref_value.data, type},
                          .access  = {.is_const       = scrutinee_type.is_constant(),
                                      .is_addressable = true,
                                      .is_poly        = scrutinee_type.is_poly()}};
    }

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
    const auto in_place{[](const place& p) {
        return match_root{
            .address = p.address,
            .access  = {.is_const = !p.is_mutable, .is_addressable = true, .is_poly = false}};
    }};
    // A named scalar is matched in place when a capture aliases it
    const bool reads_storage{inst && mod().tag_of(*inst) == tag::load};
    if (!aggregate && aliased && (names_storage || reads_storage)) {
        return in_place(TRY(address_of(operand_ref, src)));
    }

    if (aggregate) {
        if (names_storage) { return in_place(TRY(address_of(operand_ref, src))); }
        if (inst && mod().tag_of(*inst) == tag::load) {
            return in_place(TRY(address_of(mod().data_of(*inst).un.operand, src)));
        }

        // A field of named storage is matched where it lives
        if (scrutinee.is<place>() || scrutinee.is<slot_value>()) {
            return in_place(TRY(operand_address(scrutinee, src)));
        }
    }

    // A value with no storage, an aggregate one included, is matched in a copy

    const auto value{TRY(as_runtime(scrutinee, src))};
    return match_root{.address = spill(value, *value.type, false).address,
                      .access  = {.is_const = true, .is_addressable = false, .is_poly = false}};
}

auto analyzer::capture_type(const pattern::capture_site& site, capture_access root)
    -> result<gsl::not_null<gs::type*>> {
    const auto modifier{site.capture.modifier};
    if (!modifier.is_ref() && !modifier.is_ptr()) { return &site.value_type; }

    // A temporary's read-only views stay valid since its value is spilled once
    const bool wants_mutable{modifier.is_mutable_ref() || modifier.is_mutable_ptr()};
    return alias_type(modifier,
                      site.value_type,
                      {.is_const       = root.is_const || site.place_is_const,
                       .is_addressable = root.is_addressable || !wants_mutable,
                       .is_poly        = root.is_poly},
                      "value",
                      *site.capture.payload);
}

auto analyzer::alias_type(ast::type_modifier modifier,
                          gs::type&          value_type,
                          capture_access     access,
                          std::string_view   what,
                          const source&      src) -> result<gsl::not_null<gs::type*>> {
    if (!modifier.is_ref() && !modifier.is_ptr()) { return &value_type; }
    if (!access.is_addressable) {
        return comp_.report(u_,
                            src,
                            gs::error::ILLEGAL_RVALUE_CAPTURE,
                            fmt::format("Cannot capture a temporary {} by {}; only a plain "
                                        "value capture is allowed here since it has no storage "
                                        "of its own",
                                        what,
                                        modifier.is_ptr() ? "pointer" : "reference"));
    }

    const bool wants_mutable{modifier.is_mutable_ref() || modifier.is_mutable_ptr()};
    if (wants_mutable && access.is_const) {
        return comp_.report(u_,
                            src,
                            gs::error::ASSIGNMENT_TO_CONST,
                            fmt::format("Cannot capture an immutable {} by mutable {}",
                                        what,
                                        modifier.is_mutable_ref() ? "reference" : "pointer"));
    }

    if (modifier.is_poly() && !access.is_poly) {
        return comp_.report(
            u_,
            src,
            gs::error::ILLEGAL_POLY_MUTABILITY,
            fmt::format("A `{}` capture needs its {} reached through a `mut?` parameter",
                        modifier.is_poly_ref() ? "&mut?" : "^mut?",
                        what));
    }

    auto&      ctx{comp_.ctx()};
    const auto mut{*gs::types::mut::from_type_modifier(modifier)};
    return modifier.is_ref() ? &ctx.get_reference(mut, value_type)
                             : &ctx.get_pointer(mut, value_type);
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
    case pattern::step_kind::FIELD:
        // A bit-packed field has no address of its own, so a test reads a copy
        if (at.bit_packed) {
            auto&       parent_type{*m.tree.occurrences[*at.parent].value_type};
            const place whole{.address = parent, .pointee = &parent_type, .is_mutable = false};
            const auto  read{packed_read(packed_place(whole, at.index, field_type))};
            return spill(read, field_type, false).address;
        }
        break;
    case pattern::step_kind::DEREF: {
        auto& ref_type{*m.tree.occurrences[*at.parent].value_type};
        return gir::value{builder_.emit_load(gir::value{parent.data, ref_type}, ref_type),
                          field_type};
    }
    case pattern::step_kind::SLICE_BYTES: {
        auto&      slice_type{*m.tree.occurrences[*at.parent].value_type};
        auto&      ptr_type{comp_.ctx().get_pointer(
            gs::types::mut::CONSTANT, slice_type.get_data().as<gs::types::slice>().underlying)};
        const auto ptr_at{
            builder_.emit_get_element_ptr(parent, {usize_constant(SLICE_PTR_FIELD)}, ptr_type)};
        return gir::value{builder_.emit_load(gir::value{ptr_at, ptr_type}, ptr_type), ptr_type};
    }
    default: break;
    }

    return gir::value{builder_.emit_get_element_ptr(parent, {usize_constant(at.index)}, field_type),
                      field_type};
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
            if (slot->is_alias) {
                builder_.emit_store(slot->address, gir::value{address.data, *slot->type})
                    .is_initializer = true;
                continue;
            }

            const auto loaded{
                builder_.emit_load(gir::value{address.data, *slot->type}, *slot->type)};
            builder_.emit_store(slot->address, gir::value{loaded, *slot->type}).is_initializer =
                true;
        }

        const auto& arm{m.arms[d.arm]};
        if (arm.guard.empty()) {
            emit_goto(m.bodies[d.arm]);
            return {};
        }

        // A failed guard carries on to the rows after this one
        const auto on_fail{new_segment()};
        bind_slots(m, d.arm);
        const auto out{TRY(analyze_body(arm.guard))};
        if (!out.yielded) { return comp_.unsupported(u_, m.src, "a guard that yields nothing"); }
        builder_.emit_cond_goto(TRY(condition(*out.yielded, m.src)), m.bodies[d.arm], on_fail);
        switch_to(on_fail);
        return emit_decision(m, *d.fallthrough);
    }
    case pattern::decision_kind::SEQUENCE: {
        const auto       address{occurrence_address(m, d.occ)};
        auto&            subject_type{*m.tree.occurrences[d.occ].value_type};
        const gir::value subject{
            builder_.emit_load(gir::value{address.data, subject_type}, subject_type), subject_type};
        const auto test{TRY(opaque_test(subject, opaque_patterns_.at(d.test->get_index())))};
        const auto on_true{new_segment()};
        const auto on_false{new_segment()};
        builder_.emit_cond_goto(test, on_true, on_false);
        switch_to(on_true);
        TRY(emit_decision(m, d.on_true));
        switch_to(on_false);
        return emit_decision(m, d.on_false);
    }
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

auto analyzer::slot_operand(const capture_slot& slot) -> operand {
    if (slot.is_void) { return void_value(); }
    return slot_value{.address = slot.address, .type = slot.type};
}

void analyzer::bind_slots(const match_lowering& m, usize arm) {
    bind_arm_names(m.arms[arm], [&](std::string_view name) -> stdx::option<operand> {
        const auto slot{std::ranges::find_if(
            m.slots[arm], [&](const capture_slot& s) { return s.name == name; })};
        if (slot == m.slots[arm].end()) { return stdx::none; }
        return slot_operand(*slot);
    });
}

auto analyzer::emit_match(const std::vector<match_arm>&          arms,
                          const std::vector<pattern::row_input>& rows,
                          const pattern::decision_tree&          tree,
                          ref                                    operand_ref,
                          const operand&                         scrutinee,
                          gs::type&                              type,
                          const source&                          src,
                          stdx::option<u32>                      binding_else) -> result<operand> {
    TRY(require_runtime(src));
    const auto merge{new_segment()};
    const bool aggregate{
        type.get_data().is<gs::types::union_t>() || type.get_data().is<gs::types::struct_t>() ||
        type.get_data().is<gs::types::array>() || type.get_kind() == gs::type_kind::SLICE};
    const auto root{
        TRY(scrutinee_address(operand_ref, scrutinee, type, aggregate, holds_alias(rows), src))};
    match_lowering m{.tree   = tree,
                     .arms   = arms,
                     .root   = root.address,
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

            auto&      cap_type{*TRY(capture_type(site, root.access))};
            const bool is_alias{site.capture.modifier.is_ref() || site.capture.modifier.is_ptr()};
            const bool is_void{cap_type.get_kind() == gs::type_kind::VOID_};
            const auto slot{is_void ? gir::value{}
                                    : gir::value{builder_.emit_alloca(cap_type), cap_type}};
            slots.emplace_back<capture_slot>({.name     = name,
                                              .type     = &cap_type,
                                              .address  = slot,
                                              .is_void  = is_void,
                                              .is_alias = is_alias});
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
        bind_slots(m, a);
        const auto& ast_arm{
            u_.source->ast.get_as<ast::match_expr>(*src.as_opt<ast::node_id>()).arms[a]};
        const bool named_capture{ast_arm.capture &&
                                 ast_arm.capture->payload.is<ast::identifier_expr>()};
        if (const auto capture{arms[a].capture}; capture && named_capture) {
            const auto name{capture_name(*ast_arm.capture)};
            const auto slot{std::ranges::find_if(
                m.slots[a], [&](const capture_slot& s) { return s.name == name; })};
            if (slot == m.slots[a].end()) {
                return comp_.unsupported(u_, src, "this match capture");
            }

            values_.insert_or_assign(*as_inst(*capture), slot_operand(*slot));
        }

        const auto out{TRY(analyze_body(arms[a].body))};
        if (terminated()) { continue; }

        // What a destructuring binds has no value once its `else` carries on
        if (binding_else && *binding_else == a) {
            return comp_.report(u_,
                                src_of(arms[a].body.empty() ? *as_inst(arms[a].patterns.front())
                                                            : arms[a].body.front()),
                                gs::error::ILLEGAL_MATCH_PATTERN,
                                "The `else` of a destructuring can't carry on: nothing it binds "
                                "would have a value");
        }

        if (out.yielded) {
            arrivals.emplace_back<incoming>(
                {.seg = builder_.get_segment()->get_id(), .value = *out.yielded, .src = src});
        }

        emit_goto(merge);
    }

    return finish_merge(arrivals, merge, src);
}

} // namespace ghoti::uir::sema
