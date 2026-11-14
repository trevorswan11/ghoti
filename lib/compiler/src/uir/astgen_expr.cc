#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "astgen_internal.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/sema/error.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/keywords.hh"
#include "compiler/syntax/token_type.hh"
#include "compiler/uir/uir.hh"
#include "support/big_int.hh"
#include "support/diagnostic.hh"
#include "support/int128.hh"

namespace ghoti::uir::detail {

namespace {

using syntax::token_type_t;

[[nodiscard]] auto binary_tag(token_type_t op) -> stdx::option<tag> {
    switch (op) {
    case token_type_t::PLUS:          return tag::add;
    case token_type_t::MINUS:         return tag::sub;
    case token_type_t::STAR:          return tag::mul;
    case token_type_t::SLASH:         return tag::div;
    case token_type_t::PERCENT:       return tag::mod;
    case token_type_t::PLUS_PERCENT:  return tag::add_wrap;
    case token_type_t::MINUS_PERCENT: return tag::sub_wrap;
    case token_type_t::STAR_PERCENT:  return tag::mul_wrap;
    case token_type_t::PLUS_PIPE:     return tag::add_sat;
    case token_type_t::MINUS_PIPE:    return tag::sub_sat;
    case token_type_t::STAR_PIPE:     return tag::mul_sat;
    case token_type_t::SHL_PIPE:      return tag::shl_sat;
    case token_type_t::SHL_PERCENT:   return tag::shl_wrap;
    case token_type_t::SHL:           return tag::shl;
    case token_type_t::SHR:           return tag::shr;
    case token_type_t::BW_AND:        return tag::bit_and;
    case token_type_t::BW_OR:         return tag::bit_or;
    case token_type_t::CARET:         return tag::bit_xor;
    case token_type_t::EQ:            return tag::cmp_eq;
    case token_type_t::NEQ:           return tag::cmp_ne;
    case token_type_t::LT:            return tag::cmp_lt;
    case token_type_t::LT_EQ:         return tag::cmp_le;
    case token_type_t::GT:            return tag::cmp_gt;
    case token_type_t::GT_EQ:         return tag::cmp_ge;
    case token_type_t::PIPE_PIPE:     return tag::merge_enums;
    case token_type_t::PLUS_PLUS:     return tag::concat;
    default:                          return stdx::none;
    }
}

[[nodiscard]] auto unary_tag(token_type_t op) -> tag {
    switch (op) {
    case token_type_t::MINUS:         return tag::neg;
    case token_type_t::MINUS_PERCENT: return tag::neg_wrap;
    case token_type_t::BANG:          return tag::bool_not;
    case token_type_t::NOT:           return tag::bit_not;
    default:                          return tag::pos;
    }
}

[[nodiscard]] auto is_mut_reference(token_type_t t) -> bool {
    return t == token_type_t::AND_MUT || t == token_type_t::CARET_MUT ||
           t == token_type_t::AND_MUT_POLY || t == token_type_t::CARET_MUT_POLY;
}

[[nodiscard]] auto reference_flags(token_type_t t) -> u32 {
    u32 out{0};
    if (is_mut_reference(t)) { out |= flags::MUT; }
    if (t == token_type_t::AND_MUT_POLY || t == token_type_t::CARET_MUT_POLY) {
        out |= flags::POLY;
    }
    return out;
}

[[nodiscard]] auto limbs_of(u128 value) -> std::vector<u32> {
    std::vector<u32> limbs;
    for (u32 i{0}; i < 4; ++i) {
        limbs.emplace_back(static_cast<u32>((value >> (32U * i)) & u128{0xFFFFFFFFU}));
    }
    while (limbs.size() > 1 && limbs.back() == 0) { limbs.pop_back(); }
    return limbs;
}

[[nodiscard]] auto limbs_of(const big_int& value) -> std::vector<u32> {
    std::vector<u32> limbs;
    const auto       width{value.unsigned_bits().value_or(value.signed_bits())};
    for (const auto word : value.to_words(width)) {
        limbs.emplace_back(static_cast<u32>(word & 0xFFFFFFFFU));
        limbs.emplace_back(static_cast<u32>(word >> 32U));
    }
    while (limbs.size() > 1 && limbs.back() == 0) { limbs.pop_back(); }
    return limbs;
}

[[nodiscard]] auto well_known_int(std::string_view name) -> stdx::option<well_known> {
    if (name == "u8") { return well_known::u8_type; }
    if (name == "i8") { return well_known::i8_type; }
    if (name == "u16") { return well_known::u16_type; }
    if (name == "i16") { return well_known::i16_type; }
    if (name == "u32") { return well_known::u32_type; }
    if (name == "i32") { return well_known::i32_type; }
    if (name == "u64") { return well_known::u64_type; }
    if (name == "i64") { return well_known::i64_type; }
    if (name == "u128") { return well_known::u128_type; }
    if (name == "i128") { return well_known::i128_type; }
    return stdx::none;
}

[[nodiscard]] auto c_type(std::string_view name) -> stdx::option<well_known> {
    if (name == "c_char") { return well_known::c_char_type; }
    if (name == "c_short") { return well_known::c_short_type; }
    if (name == "c_ushort") { return well_known::c_ushort_type; }
    if (name == "c_int") { return well_known::c_int_type; }
    if (name == "c_uint") { return well_known::c_uint_type; }
    if (name == "c_long") { return well_known::c_long_type; }
    if (name == "c_ulong") { return well_known::c_ulong_type; }
    if (name == "c_longlong") { return well_known::c_longlong_type; }
    if (name == "c_ulonglong") { return well_known::c_ulonglong_type; }
    if (name == "c_longdouble") { return well_known::c_longdouble_type; }
    return stdx::none;
}

[[nodiscard]] auto keyword_type(token_type_t t) -> stdx::option<well_known> {
    switch (t) {
    case token_type_t::ISIZE_TYPE:          return well_known::isize_type;
    case token_type_t::USIZE_TYPE:          return well_known::usize_type;
    case token_type_t::F16_TYPE:            return well_known::f16_type;
    case token_type_t::F32_TYPE:            return well_known::f32_type;
    case token_type_t::F64_TYPE:            return well_known::f64_type;
    case token_type_t::F80_TYPE:            return well_known::f80_type;
    case token_type_t::F128_TYPE:           return well_known::f128_type;
    case token_type_t::COMPTIME_INT_TYPE:   return well_known::comptime_int_type;
    case token_type_t::COMPTIME_FLOAT_TYPE: return well_known::comptime_float_type;
    case token_type_t::BOOL_TYPE:           return well_known::bool_type;
    case token_type_t::VOID_TYPE:           return well_known::void_type;
    case token_type_t::TYPE_TYPE:           return well_known::type_type;
    case token_type_t::AUTO_TYPE:           return well_known::auto_type;
    case token_type_t::OPAQUE_TYPE:         return well_known::opaque_type;
    case token_type_t::NORETURN:            return well_known::noreturn_type;
    case token_type_t::UNDEFINED:           return well_known::undefined_type;
    case token_type_t::NULLPTR:             return well_known::nullptr_type;
    default:                                return stdx::none;
    }
}

// A pattern node that refines rather than compares
[[nodiscard]] auto is_structural_pattern(ast::node_id id) -> bool {
    using ast::node_kind;
    switch (id.get_kind()) {
    case node_kind::DISCARDED:
    case node_kind::CAPTURE_PATTERN_EXPRESSION:
    case node_kind::BOUND_PATTERN_EXPRESSION:
    case node_kind::INITIALIZER_EXPRESSION:
    case node_kind::RANGE_EXPRESSION:           return true;
    default:                                    return false;
    }
}

} // namespace

// By spelling, since no user name can be spelled like a primitive type and some positions hand
// a primitive over as a plain identifier
auto astgen::primitive_named(std::string_view name, token_type_t type, source src)
    -> stdx::option<ref> {
    if (syntax::token_type::is_int_type_lexeme(name)) {
        if (const auto known{well_known_int(name)}) { return to_ref(*known); }
        const bool is_signed{name.front() == 'i'};
        u32        bits{0};
        for (const char c : name.substr(1)) { bits = bits * 10 + static_cast<u32>(c - '0'); }
        return add_pl(tag::int_type, writer().u32_field(bits).u32_field(is_signed ? 1U : 0U), src);
    }
    if (const auto known{c_type(name)}) { return to_ref(*known); }
    if (const auto known{keyword_type(type)}) { return to_ref(*known); }
    if (const auto keyword{syntax::get_keyword_opt(name)}) {
        if (const auto known{keyword_type(*keyword)}) { return to_ref(*known); }
    }
    return stdx::none;
}

auto astgen::identifier(source src, source_location at, std::string_view name, use u)
    -> place_result {
    const auto found{lookup(name)};
    if (const auto hit{found.as_opt<lookup_hit>()}) {
        const auto& b{hit->found};
        switch (b.kind) {
        case binding_kind::DECL:
            if (u != use::VALUE && b.mutable_decl) {
                return {add_un(tag::decl_ref, stdx::none, std::to_underlying(*b.decl), src), true};
            }
            return {add_un(tag::decl_val, stdx::none, std::to_underlying(*b.decl), src), false};
        case binding_kind::FIELD:
            report(at,
                   sema::error::UNDECLARED_IDENTIFIER,
                   "'{0}' is a field; access it through an instance, e.g. `self.{0}`",
                   name);
            return {to_ref(well_known::undefined), false};
        case binding_kind::LABEL: break;
        case binding_kind::VALUE:
        case binding_kind::PLACE: {
            const bool is_place{b.kind == binding_kind::PLACE};
            if (!hit->crossed.empty()) {
                const bool loaded{is_place && u == use::VALUE};
                return {capture_ref(*hit, u, src), is_place && !loaded};
            }
            if (is_place && u == use::VALUE) { return {add_un(tag::load, b.value, 0, src), false}; }
            return {b.value, is_place};
        }
        }
    }
    if (const auto early{found.as_opt<used_before_declaration>()}) {
        report(at,
               sema::error::ILLEGAL_FIELD_ORDER_DEPENDENCY,
               "'{}' is used before its declaration",
               name);
        return {to_ref(well_known::undefined), false};
    }
    if (name == "builtin") { return {to_ref(well_known::builtin_module), false}; }
    if (prunable_depth_ > 0) { return {add_str(tag::undeclared, name, src), false}; }
    report(at, sema::error::UNDECLARED_IDENTIFIER, "Use of undeclared identifier '{}'", name);
    return {to_ref(well_known::undefined), false};
}

auto astgen::identifier_node(ast::node_id id, use u) -> place_result {
    const auto& ident{ast_.get_as<ast::identifier_expr>(id)};
    const auto  token{id.get_token_type()};
    if (const auto prim{primitive_named(ident.name, token, id)}) { return {*prim, false}; }
    if (const auto builtin{id.get_builtin()}) {
        return {add_un(tag::builtin_ref, {}, static_cast<u32>(*builtin), id), false};
    }
    return identifier(id, location_of(id), ident.name, u);
}

auto astgen::expr(ast::expr_handle h) -> ref {
    const ast::node_id id{h};
    using ast::node_kind;
    switch (id.get_kind()) {
    case node_kind::IDENTIFIER_EXPRESSION: return identifier_node(id, use::VALUE).value;
    case node_kind::STRING_EXPRESSION:
        return add_str(tag::str, ast_.get_as<ast::string_expr>(id).value, id);
    case node_kind::INT_LITERAL_EXPRESSION:   return int_literal(id);
    case node_kind::FLOAT_LITERAL_EXPRESSION: {
        const auto& lit{ast_.get_as<ast::float_literal_expr>(id)};
        return add_pl(tag::float_lit,
                      writer().words_field(limbs_of(lit.value.bits())).u32_field(lit.width),
                      id);
    }
    case node_kind::BOOL_EXPRESSION:
        return to_ref(id.get_token_type() == token_type_t::BOOLEAN_TRUE ? well_known::bool_true
                                                                        : well_known::bool_false);
    case node_kind::VOID_EXPRESSION:        return to_ref(well_known::void_value);
    case node_kind::UNDEFINED_EXPRESSION:   return to_ref(well_known::undefined);
    case node_kind::NULLPTR_EXPRESSION:     return to_ref(well_known::nullptr_value);
    case node_kind::UNREACHABLE_EXPRESSION: return add(tag::unreachable, {}, id);
    case node_kind::IMPLICIT_ACCESS_EXPRESSION:
        return add_str(
            tag::implicit_access, name_of(ast_.get_as<ast::implicit_access_expr>(id).member), id);
    case node_kind::COMPTIME_EXPRESSION: {
        std::vector<inst_id> body;
        {
            const body_guard guard{*this, body};
            yields(expr(ast_.get_as<ast::comptime_expr>(id).rhs), id);
        }
        return add_pl(tag::block_comptime, writer().str_field({}).body_field(body), id);
    }
    case node_kind::REFERENCE_EXPRESSION:
    case node_kind::ADDRESS_OF_EXPRESSION: {
        const auto rhs{id.is<ast::reference_expr>() ? ast_.get_as<ast::reference_expr>(id).rhs
                                                    : ast_.get_as<ast::address_of_expr>(id).rhs};
        const auto bits{reference_flags(id.get_token_type())};
        const auto operand{place(rhs, (bits & flags::MUT) != 0 ? use::MUTATE : use::PLACE)};
        return add_pl(id.is<ast::reference_expr>() ? tag::ref_of : tag::address_of,
                      writer()
                          .ref_field(operand.value)
                          .u32_field(bits | (operand.is_place ? flags::PLACE : 0U)),
                      id);
    }
    case node_kind::DEREFERENCE_EXPRESSION:
        return add_un(tag::deref, expr(ast_.get_as<ast::dereference_expr>(id).rhs), 0, id);
    case node_kind::UNARY_EXPRESSION:
        return add_un(
            unary_tag(id.get_token_type()), expr(ast_.get_as<ast::unary_expr>(id).rhs), 0, id);
    case node_kind::STRUCT_EXPRESSION: return struct_decl(id, ast_.get_as<ast::struct_expr>(id));
    case node_kind::UNION_EXPRESSION:  return union_decl(id, ast_.get_as<ast::union_expr>(id));
    case node_kind::ENUM_EXPRESSION:   return enum_decl(id, ast_.get_as<ast::enum_expr>(id));
    case node_kind::INTERFACE_EXPRESSION:
        return interface_decl(id, ast_.get_as<ast::interface_expr>(id));
    case node_kind::TYPE_EXPRESSION:            return type(ast_.get_as<ast::type_expr>(id).type);
    case node_kind::CAPTURE_PATTERN_EXPRESSION:
    case node_kind::BOUND_PATTERN_EXPRESSION:
        UNREACHABLE("the parser only builds patterns inside a match arm");
    default: break;
    }
    return ast_[id].visit([&](const auto& node) -> ref {
        if constexpr (requires { lower_node(id, node); }) {
            return lower_node(id, node);
        } else {
            UNREACHABLE("every expression kind has a lowering");
        }
    });
}

auto astgen::int_literal(ast::node_id id) -> ref {
    const auto& lit{ast_.get_as<ast::int_literal_expr>(id)};
    const bool  plain{!lit.is_big && lit.width == 0 && !lit.is_signed && !lit.is_size &&
                      !lit.is_char && lit.value <= u128{~u64{0}}};
    if (plain) {
        data d{};
        d.int_value = static_cast<u64>(lit.value);
        return add(tag::int_value, d, id);
    }
    u32 bits{0};
    if (lit.is_signed) { bits |= flags::INT_SIGNED; }
    if (lit.is_size) { bits |= flags::INT_SIZE; }
    if (lit.is_char) { bits |= flags::INT_CHAR; }
    if (lit.is_big) { bits |= flags::INT_BIG; }
    const auto limbs{lit.is_big ? limbs_of(lit.exact_value()) : limbs_of(lit.value)};
    return add_pl(
        tag::int_lit, writer().words_field(limbs).u32_field(lit.width).u32_field(bits), id);
}

auto astgen::optional_expr(stdx::option<ast::expr_handle> h) -> stdx::option<ref> {
    if (!h) { return stdx::none; }
    return expr(*h);
}

auto astgen::place(ast::expr_handle h, use u) -> place_result {
    const ast::node_id id{h};
    const auto         inner{u == use::MUTATE ? use::MUTATE : use::PLACE};
    if (id.is<ast::identifier_expr>()) { return identifier_node(id, u); }
    if (const auto dot{ast_.get_as_opt<ast::dot_expr>(id)}) {
        const auto object{place(dot->object, inner)};
        return {add_pl(tag::field_ptr,
                       writer()
                           .ref_field(object.value)
                           .str_field(name_of(dot->member))
                           .u32_field(object.is_place ? flags::PLACE : 0U),
                       id),
                true};
    }
    if (const auto index{ast_.get_as_opt<ast::index_expr>(id)};
        index && !index->index.is<ast::range_expr>()) {
        const auto object{place(index->array, inner)};
        const auto at{expr(index->index)};
        return {add_pl(tag::elem_ptr,
                       writer()
                           .ref_field(object.value)
                           .ref_field(at)
                           .u32_field(object.is_place ? flags::PLACE : 0U),
                       id),
                true};
    }
    if (const auto deref{ast_.get_as_opt<ast::dereference_expr>(id)}) {
        return {expr(deref->rhs), true};
    }
    return {expr(h), false};
}

auto astgen::lower_node(ast::node_id id, const ast::binary_expr& node) -> ref {
    const auto op{id.get_token_type()};
    if (op == token_type_t::BOOLEAN_AND || op == token_type_t::BOOLEAN_OR) {
        const auto lhs{expr(node.lhs)};
        return add_pl(op == token_type_t::BOOLEAN_AND ? tag::bool_and : tag::bool_or,
                      writer().ref_field(lhs).body_field(value_body(node.rhs)),
                      id);
    }
    const auto lhs{expr(node.lhs)};
    const auto rhs{expr(node.rhs)};
    const auto t{binary_tag(op)};
    ASSERT(t, "every binary operator the parser accepts has a UIR tag");
    return add_bin(*t, lhs, rhs, id);
}

auto astgen::lower_node(ast::node_id id, const ast::assignment_expr& node) -> ref {
    const auto target{place(node.lhs, use::MUTATE)};
    const auto op{id.get_token_type()};
    ref        value{};
    if (const auto base{syntax::token_type::get_compound_base_op(op)}) {
        const auto current{target.is_place ? add_un(tag::load, target.value, 0, id) : target.value};
        const auto rhs{expr(node.rhs)};
        const auto t{binary_tag(*base)};
        ASSERT(t, "every compound assignment's operator has a UIR tag");
        value = add_bin(*t, current, rhs, id);
    } else {
        value = expr(node.rhs);
    }
    DISCARD(add_bin(tag::store, target.value, value, id));
    return to_ref(well_known::void_value);
}

auto astgen::call_args(const ast::call_expr& call)
    -> std::pair<std::vector<ref>, std::vector<u32>> {
    std::vector<ref> args;
    std::vector<u32> pack_mask((call.arguments.size() + 31) / 32, 0);
    for (usize i{0}; i < call.arguments.size(); ++i) {
        args.emplace_back(
            call.arguments[i].visit([this](ast::expr_handle e) { return expr(e); },
                                    [this](ast::explicit_type_id t) { return type(t); }));
        if (i < call.pack_expansions.size() && call.pack_expansions[i]) {
            pack_mask[i / 32] |= 1U << (i % 32);
        }
    }
    return {std::move(args), std::move(pack_mask)};
}

auto astgen::call(source src, source_location, const ast::call_expr& node) -> ref {
    const ast::node_id callee{node.function};
    if (const auto builtin{callee.get_builtin()}; builtin && callee.is<ast::identifier_expr>()) {
        return builtin_call(src, callee, *builtin, node);
    }

    const auto modifier{optional_expr(node.modifier)};
    if (const auto dot{ast_.get_as_opt<ast::dot_expr>(callee)}) {
        const auto object{place(dot->object, use::PLACE)};
        auto [args, mask]{call_args(node)};
        return add_pl(tag::field_call,
                      writer()
                          .ref_field(object.value)
                          .str_field(name_of(dot->member))
                          .u32_field(object.is_place ? flags::PLACE : 0U)
                          .ref_field(modifier)
                          .refs_field(args)
                          .words_field(mask),
                      src);
    }
    const auto function{expr(node.function)};
    auto [args, mask]{call_args(node)};
    return add_pl(
        tag::call,
        writer().ref_field(function).ref_field(modifier).refs_field(args).words_field(mask),
        src);
}

auto astgen::lower_node(ast::node_id id, const ast::call_expr& node) -> ref {
    return call(id, location_of(id), node);
}

auto astgen::builtin_call(source                src,
                          ast::node_id          callee,
                          syntax::builtin_t     builtin,
                          const ast::call_expr& node) -> ref {
    // `@expect`, `@require`, and `@skip` only make sense in a test or a `@[testing]` helper
    using syntax::builtin_t;
    const bool test_only{builtin == builtin_t::EXPECT || builtin == builtin_t::REQUIRE ||
                         builtin == builtin_t::SKIP};
    const bool testing{!functions_.empty() && functions_.back().testing};
    if (test_only && !testing && !in_test()) {
        report(location_of(callee),
               sema::error::TEST_BUILTIN_OUTSIDE_TEST,
               "'{}' may only be used inside a 'test' block or a `@[testing]` function",
               syntax::builtin_name(builtin));
    }
    auto [args, mask]{call_args(node)};
    return add_pl(
        tag::builtin, writer().u32_field(static_cast<u32>(builtin)).refs_field(args), src);
}

auto astgen::lower_node(ast::node_id id, const ast::dot_expr& node) -> ref {
    const auto object{place(node.object, use::PLACE)};
    if (object.is_place) {
        const auto ptr{add_pl(tag::field_ptr,
                              writer()
                                  .ref_field(object.value)
                                  .str_field(name_of(node.member))
                                  .u32_field(flags::PLACE),
                              id)};
        return add_un(tag::load, ptr, 0, id);
    }
    return add_pl(
        tag::field_val, writer().ref_field(object.value).str_field(name_of(node.member)), id);
}

auto astgen::lower_node(ast::node_id id, const ast::index_expr& node) -> ref {
    if (const auto range{ast_.get_as_opt<ast::range_expr>(node.index)}) {
        const auto object{place(node.array, use::PLACE)};
        const auto lo{optional_expr(range->lhs)};
        const auto hi{optional_expr(range->rhs)};
        u32        bits{object.is_place ? flags::PLACE : 0U};
        if (ast::node_id{node.index}.get_token_type() == token_type_t::DOT_DOT_EQ) {
            bits |= flags::INCLUSIVE;
        }
        return add_pl(tag::slice,
                      writer().ref_field(object.value).ref_field(lo).ref_field(hi).u32_field(bits),
                      id);
    }
    const auto object{place(node.array, use::PLACE)};
    const auto at{expr(node.index)};
    if (object.is_place) {
        const auto ptr{
            add_pl(tag::elem_ptr,
                   writer().ref_field(object.value).ref_field(at).u32_field(flags::PLACE),
                   id)};
        return add_un(tag::load, ptr, 0, id);
    }
    return add_bin(tag::elem_val, object.value, at, id);
}

auto astgen::lower_node(ast::node_id id, const ast::range_expr& node) -> ref {
    const auto lo{optional_expr(node.lhs)};
    const auto hi{optional_expr(node.rhs)};
    const u32  bits{id.get_token_type() == token_type_t::DOT_DOT_EQ ? flags::INCLUSIVE : 0U};
    return add_pl(tag::range, writer().ref_field(lo).ref_field(hi).u32_field(bits), id);
}

auto astgen::lower_node(ast::node_id id, const ast::initializer_expr& node) -> ref {
    const auto type_ref{optional_expr(node.object_type)};
    const bool positional{!node.initializers.empty() &&
                          std::ranges::none_of(node.initializers, [](const auto& init) {
                              return init.member.has_value();
                          })};
    if (positional) {
        std::vector<ref> values;
        for (const auto& init : node.initializers) { values.emplace_back(expr(init.value)); }
        return add_pl(tag::init_array, writer().ref_field(type_ref).refs_field(values), id);
    }
    std::vector<std::pair<std::string_view, ref>> fields;
    for (const auto& init : node.initializers) {
        const auto name{init.member
                            ? name_of(ast_.get_as<ast::implicit_access_expr>(*init.member).member)
                            : std::string_view{}};
        fields.emplace_back(name, expr(init.value));
    }
    auto record{writer()};
    record.ref_field(type_ref).count(fields.size());
    for (const auto& [name, value] : fields) { record.str_field(name).ref_field(value); }
    return add_pl(tag::init_struct, record, id);
}

auto astgen::lower_node(ast::node_id id, const ast::array_expr& node) -> ref {
    u32 bits{0};
    if (node.mut_elements) { bits |= flags::MUT; }
    if (node.null_terminated) { bits |= flags::SENTINEL; }
    if (node.poly_elements) { bits |= flags::POLY; }
    if (node.is_type_expr) {
        const auto elem{type(node.item_explicit_type)};
        if (!node.size) {
            return add_pl(tag::slice_type, writer().ref_field(elem).u32_field(bits), id);
        }
        const auto len{expr(*node.size)};
        return add_pl(tag::array_type, writer().ref_field(len).ref_field(elem).u32_field(bits), id);
    }
    const auto len{optional_expr(node.size)};
    if (!node.size) { bits |= flags::LEN_ABSENT; }
    const auto       elem{type(node.item_explicit_type)};
    std::vector<ref> items;
    for (const auto item : node.items) { items.emplace_back(expr(item)); }
    return add_pl(tag::array_lit,
                  writer().ref_field(len).ref_field(elem).u32_field(bits).refs_field(items),
                  id);
}

auto astgen::value_body(ast::expr_handle h) -> std::vector<inst_id> {
    std::vector<inst_id> body;
    const body_guard     guard{*this, body};
    yields(expr(h), ast::node_id{h});
    return body;
}

auto astgen::arm_value(ast::stmt_handle arm) -> std::vector<inst_id> {
    std::vector<inst_id> body;
    const body_guard     guard{*this, body};
    const ast::node_id   id{arm};
    if (const auto expr_stmt{ast_.get_as_opt<ast::expr_stmt>(id)}) {
        yields(expr(expr_stmt->expression), id);
        return body;
    }
    if (id.is<ast::block_stmt>()) {
        const auto inner{scoped_block(ast::block_handle{id})};
        const auto block{add_pl(tag::block, writer().str_field({}).body_field(inner), id)};
        DISCARD(block);
        yields(to_ref(well_known::void_value), id);
        return body;
    }
    if (!stmt(arm)) { yields(to_ref(well_known::void_value), id); }
    return body;
}

auto astgen::bind_capture(const ast::capture_t& capture, u32 slot, source src) -> void {
    const auto name{ast_.get_as_opt<ast::identifier_expr>(capture.payload)};
    const auto bound{add_pl(tag::bind,
                            writer()
                                .str_field(name ? name->name : std::string_view{})
                                .u32_field(slot)
                                .u32_field(static_cast<u32>(capture.modifier.get_raw())),
                            src)};
    if (!name) { return; }
    DISCARD(declare(
        name->name, location_of(*capture.payload), {.kind = binding_kind::VALUE, .value = bound}));
}

auto astgen::lower_node(ast::node_id id, const ast::if_expr& node) -> ref {
    if (node.is_evaluation_context_branch()) {
        const auto           comptime_arm{arm_value(node.consequence)};
        std::vector<inst_id> runtime_arm;
        if (node.alternate) {
            runtime_arm = arm_value(*node.alternate);
        } else {
            const body_guard guard{*this, runtime_arm};
            yields(to_ref(well_known::void_value), id);
        }
        return add_pl(
            tag::eval_branch, writer().body_field(comptime_arm).body_field(runtime_arm), id);
    }

    const auto condition{expr(*node.condition)};
    const auto captured_arm{
        [&](const stdx::option<ast::capture_t>& capture, ast::stmt_handle arm, u32 slot) {
            if (!capture) { return arm_value(arm); }
            std::vector<inst_id> body;
            const body_guard     guard{*this, body};
            const scope_guard    scoped{*this, scope_kind::BLOCK};
            bind_capture(*capture, slot, id);
            auto inner{arm_value(arm)};
            body.insert(body.end(), inner.begin(), inner.end());
            return body;
        }};
    const auto void_arm{[&] {
        std::vector<inst_id> body;
        const body_guard     guard{*this, body};
        yields(to_ref(well_known::void_value), id);
        return body;
    }};

    if (node.payload_capture) {
        const auto then_arm{captured_arm(node.payload_capture, node.consequence, 0)};
        const auto else_arm{node.alternate ? captured_arm(node.else_capture, *node.alternate, 1)
                                           : void_arm()};
        return add_pl(tag::unwrap_branch,
                      writer().ref_field(condition).body_field(then_arm).body_field(else_arm),
                      id);
    }
    const prunable_guard prunable{*this, node.comptime_condition};
    const auto           then_arm{arm_value(node.consequence)};
    const auto           else_arm{node.alternate ? arm_value(*node.alternate) : void_arm()};
    return add_pl(node.comptime_condition ? tag::condbr_comptime : tag::condbr,
                  writer().ref_field(condition).body_field(then_arm).body_field(else_arm),
                  id);
}

auto astgen::pattern(ast::node_id id, pattern_slots& slots) -> ref {
    const auto capture_of{[&](const ast::capture_t& capture, ast::node_id at) -> stdx::option<ref> {
        const auto              ident{ast_.get_as_opt<ast::identifier_expr>(capture.payload)};
        const stdx::option<ref> sub{
            capture.pattern ? stdx::option<ref>{pattern(ast::node_id{*capture.pattern}, slots)}
                            : stdx::none};
        if (!ident) {
            if (!capture.pattern) { return stdx::none; }
            return sub;
        }
        if (!slots.alternative.insert(ident->name).second) {
            report(location_of(*capture.payload),
                   sema::error::ILLEGAL_MATCH_PATTERN,
                   "Name '{}' is bound twice in this pattern",
                   ident->name);
        }
        const auto [slot, added]{
            slots.by_name.try_emplace(ident->name, static_cast<u32>(slots.by_name.size()))};
        if (added) {
            slots.order.emplace_back<named_capture>(
                {ident->name, capture, location_of(*capture.payload)});
        }
        return add_pl(tag::pat_capture,
                      writer()
                          .str_field(ident->name)
                          .u32_field(slot->second)
                          .u32_field(static_cast<u32>(capture.modifier.get_raw()))
                          .ref_field(sub),
                      at);
    }};

    if (id.get_kind() == ast::node_kind::DISCARDED) { return add(tag::pat_wildcard, {}, id); }
    if (const auto capture{ast_.get_as_opt<ast::capture_pattern_expr>(id)}) {
        if (capture->is_bare_discard) { return add(tag::pat_wildcard, {}, id); }
        if (const auto bound{capture_of(capture->capture, id)}) { return *bound; }
        return add(tag::pat_wildcard, {}, id);
    }
    if (const auto bound{ast_.get_as_opt<ast::bound_pattern_expr>(id)}) {
        const auto inner{pattern(ast::node_id{bound->pattern}, slots)};
        const auto cap{capture_of(bound->capture, id)};
        return add_bin(tag::pat_bound, inner, cap, id);
    }
    if (const auto range{ast_.get_as_opt<ast::range_expr>(id)}) {
        const auto lo{range->lhs ? value_body(*range->lhs) : std::vector<inst_id>{}};
        const auto hi{range->rhs ? value_body(*range->rhs) : std::vector<inst_id>{}};
        const u32  bits{id.get_token_type() == token_type_t::DOT_DOT_EQ ? flags::INCLUSIVE : 0U};
        return add_pl(tag::pat_range, writer().body_field(lo).body_field(hi).u32_field(bits), id);
    }
    if (const auto init{ast_.get_as_opt<ast::initializer_expr>(id)}) {
        const auto type_body_insts{init->object_type ? value_body(*init->object_type)
                                                     : std::vector<inst_id>{}};
        std::vector<std::pair<std::string_view, ref>> entries;
        for (const auto& entry : init->initializers) {
            const auto name{
                entry.member ? name_of(ast_.get_as<ast::implicit_access_expr>(*entry.member).member)
                             : std::string_view{}};
            entries.emplace_back(name, pattern(ast::node_id{entry.value}, slots));
        }
        auto record{writer()};
        record.body_field(type_body_insts).count(entries.size());
        for (const auto& [name, sub] : entries) { record.str_field(name).ref_field(sub); }
        return add_pl(tag::pat_init, record, id);
    }
    if (const auto ref_pattern{ast_.get_as_opt<ast::reference_expr>(id)};
        ref_pattern && is_structural_pattern(ast::node_id{ref_pattern->rhs})) {
        return add_un(tag::pat_deref, pattern(ast::node_id{ref_pattern->rhs}, slots), 0, id);
    }
    if (const auto call_pattern{ast_.get_as_opt<ast::call_expr>(id)};
        call_pattern && call_pattern->function.any<ast::implicit_access_expr, ast::dot_expr>()) {
        const auto       callee{value_body(call_pattern->function)};
        std::vector<ref> args;
        for (const auto& arg : call_pattern->arguments) {
            const auto value{arg.as_opt<ast::expr_handle>()};
            if (!value) { continue; }
            const ast::node_id arg_id{*value};
            if (is_structural_pattern(arg_id) || arg_id.is<ast::call_expr>()) {
                args.emplace_back(pattern(arg_id, slots));
            } else {
                args.emplace_back(
                    add_pl(tag::pat_value, writer().body_field(value_body(*value)), arg_id));
            }
        }
        return add_pl(tag::pat_call, writer().body_field(callee).refs_field(args), id);
    }
    return add_pl(tag::pat_value, writer().body_field(value_body(ast::expr_handle{id})), id);
}

auto astgen::lower_node(ast::node_id id, const ast::match_expr& node) -> ref {
    const auto operand{expr(node.matcher)};
    u32        bits{0};
    if (node.is_comptime) { bits |= flags::MATCH_COMPTIME; }
    if (node.is_binding) { bits |= flags::MATCH_BINDING; }

    struct arm_record {
        std::vector<ref>     roots;
        std::vector<inst_id> pattern_body;
        ref                  capture{};
        std::vector<inst_id> guard;
        std::vector<inst_id> body;
    };
    // A match on a type value prunes like `match comptime`, and only Sema knows the operand's type
    std::vector<arm_record> arms;
    const prunable_guard    prunable{*this, true};
    for (const auto& arm : node.arms) {
        const auto        empty_body{ast_.get_as_opt<ast::block_stmt>(arm.dispatch)};
        const bool        checks_destructuring{node.is_binding && empty_body &&
                                               empty_body->statements.empty()};
        const scope_guard scoped{*this, scope_kind::BLOCK};
        arm_record        record;
        pattern_slots     slots;
        {
            const body_guard guard{*this, record.pattern_body};
            for (const auto alternative : arm.patterns) {
                slots.alternative.clear();
                record.roots.emplace_back(pattern(ast::node_id{alternative}, slots));
            }
            // One binding per name, shared by every alternative that binds it
            for (u32 slot{0}; slot < slots.order.size(); ++slot) {
                const auto& named{slots.order[slot]};
                const auto  bound{
                    add_pl(tag::bind,
                           writer()
                               .str_field(named.name)
                               .u32_field(slot)
                               .u32_field(static_cast<u32>(named.capture.modifier.get_raw())),
                           id)};
                // A destructuring's check, with its empty body, binds nothing; the declarations
                // after it do. A capture's pattern binds into the body it wraps
                if (!checks_destructuring) {
                    DISCARD(declare(
                        named.name, named.at, {.kind = binding_kind::VALUE, .value = bound}));
                }
            }
            if (arm.capture) {
                const auto slot{static_cast<u32>(slots.order.size())};
                bind_capture(*arm.capture, slot, id);
                record.capture = inst_ref(record.pattern_body.back());
            }
        }
        if (arm.guard) { record.guard = value_body(*arm.guard); }
        record.body = arm_value(arm.dispatch);
        arms.emplace_back(std::move(record));
    }

    auto record{writer()};
    record.ref_field(operand)
        .u32_field(bits)
        .index_field(node.catch_all_idx ? stdx::option<u32>{static_cast<u32>(*node.catch_all_idx)}
                                        : stdx::none)
        .count(arms.size());
    for (const auto& arm : arms) {
        record.refs_field(arm.roots)
            .body_field(arm.pattern_body)
            .ref_field(arm.capture)
            .body_field(arm.guard)
            .body_field(arm.body);
    }
    return add_pl(tag::match, record, id);
}

auto astgen::lower_node(ast::node_id id, const ast::label_expr& node) -> ref {
    const scope_guard              scoped{*this, scope_kind::BLOCK};
    stdx::option<std::string_view> label;
    if (node.name) {
        label.emplace(name_of(*node.name));
        DISCARD(declare(*label, location_of(id), {.kind = binding_kind::LABEL}));
    }
    const ast::node_id body{node.body};
    if (body.any<ast::while_loop_expr,
                 ast::do_while_loop_expr,
                 ast::infinite_loop_expr,
                 ast::for_loop_expr>()) {
        pending_label_ = label;
        return expr(ast::expr_handle{body});
    }

    const auto inst{reserve(id)};
    functions_.back().targets.emplace_back<jump_target>({
        .inst         = inst,
        .label        = label,
        .is_loop      = false,
        .defer_height = functions_.back().defers.size(),
    });
    std::vector<inst_id> instructions;
    if (body.is<ast::block_stmt>()) {
        instructions = scoped_block(ast::block_handle{body});
    } else {
        const body_guard guard{*this, instructions};
        DISCARD(add_bin(tag::break_to, inst_ref(inst), expr(ast::expr_handle{body}), id));
    }
    functions_.back().targets.pop_back();
    const bool comptime_label{node.is_comptime(ast_)};
    fill_pl(inst,
            comptime_label ? tag::block_comptime : tag::block,
            writer().str_field(label.value_or(std::string_view{})).body_field(instructions));
    return inst_ref(inst);
}

auto astgen::begin_loop(ast::node_id id, bool is_loop) -> inst_id {
    const auto inst{reserve(id)};
    functions_.back().targets.emplace_back<jump_target>({
        .inst         = inst,
        .label        = std::exchange(pending_label_, stdx::none),
        .is_loop      = is_loop,
        .defer_height = functions_.back().defers.size(),
    });
    return inst;
}

auto astgen::loop_label() const -> std::string_view {
    return functions_.back().targets.back().label.value_or(std::string_view{});
}

auto astgen::lower_node(ast::node_id id, const ast::while_loop_expr& node) -> ref {
    std::vector<inst_id> cond;
    {
        const body_guard guard{*this, cond};
        yields(expr(node.condition), id);
    }
    const auto inst{begin_loop(id, true)};
    const auto label{loop_label()};

    // The payload capture is the body's own name, so it shares the body's scope
    std::vector<inst_id> body;
    std::vector<inst_id> cont;
    {
        const scope_guard scoped{*this, scope_kind::BLOCK};
        {
            const body_guard guard{*this, body};
            if (node.payload_capture) { bind_capture(*node.payload_capture, 0, id); }
        }
        if (node.continuation) {
            const body_guard guard{*this, cont};
            DISCARD(expr(*node.continuation));
        }
        auto statements{block_in_current_scope(node.block)};
        body.insert(body.end(), statements.begin(), statements.end());
    }

    // The `else` belongs to the loop, so a labeled break in it still lands on the loop
    std::vector<inst_id> otherwise;
    if (node.non_break) {
        const scope_guard scoped{*this, scope_kind::BLOCK};
        const body_guard  guard{*this, otherwise};
        if (node.else_capture) { bind_capture(*node.else_capture, 1, id); }
        auto value{arm_value(*node.non_break)};
        otherwise.insert(otherwise.end(), value.begin(), value.end());
    }
    functions_.back().targets.pop_back();

    u32 bits{flags::HAS_COND};
    if (node.is_unrolled) { bits |= flags::UNROLL; }
    if (node.payload_capture) { bits |= flags::UNWRAPS; }
    fill_pl(inst,
            tag::loop,
            writer()
                .str_field(label)
                .u32_field(bits)
                .body_field(cond)
                .body_field(body)
                .body_field(cont)
                .body_field(otherwise));
    return inst_ref(inst);
}

auto astgen::lower_node(ast::node_id id, const ast::do_while_loop_expr& node) -> ref {
    const auto inst{begin_loop(id, true)};
    const auto label{loop_label()};
    const auto body{scoped_block(node.block)};
    functions_.back().targets.pop_back();
    std::vector<inst_id> cond;
    {
        const body_guard guard{*this, cond};
        yields(expr(node.condition), id);
    }
    u32 bits{flags::HAS_COND | flags::DO_WHILE};
    if (node.is_unrolled) { bits |= flags::UNROLL; }
    fill_pl(inst,
            tag::loop,
            writer()
                .str_field(label)
                .u32_field(bits)
                .body_field(cond)
                .body_field(body)
                .body_field({})
                .body_field({}));
    return inst_ref(inst);
}

auto astgen::lower_node(ast::node_id id, const ast::infinite_loop_expr& node) -> ref {
    const auto inst{begin_loop(id, true)};
    const auto label{loop_label()};
    const auto body{scoped_block(node.block)};
    functions_.back().targets.pop_back();
    const u32 bits{node.is_unrolled ? flags::UNROLL : 0U};
    fill_pl(inst,
            tag::loop,
            writer()
                .str_field(label)
                .u32_field(bits)
                .body_field({})
                .body_field(body)
                .body_field({})
                .body_field({}));
    return inst_ref(inst);
}

auto astgen::lower_node(ast::node_id id, const ast::for_loop_expr& node) -> ref {
    std::vector<ref> iterables;
    for (const auto iterable : node.iterables) { iterables.emplace_back(expr(iterable)); }
    const auto inst{begin_loop(id, true)};
    const auto label{loop_label()};

    std::vector<inst_id> body;
    {
        const scope_guard scoped{*this, scope_kind::BLOCK};
        {
            const body_guard guard{*this, body};
            for (u32 slot{0}; slot < node.captures.size(); ++slot) {
                bind_capture(node.captures[slot], slot, id);
            }
        }
        auto statements{block_in_current_scope(node.block)};
        body.insert(body.end(), statements.begin(), statements.end());
    }
    const auto otherwise{node.non_break ? arm_value(*node.non_break) : std::vector<inst_id>{}};
    functions_.back().targets.pop_back();
    const u32 bits{node.is_unrolled ? flags::UNROLL : 0U};
    fill_pl(
        inst,
        tag::for_iter,
        writer().str_field(label).u32_field(bits).refs_field(iterables).body_field(body).body_field(
            otherwise));
    return inst_ref(inst);
}

auto astgen::lower_node(ast::node_id id, const ast::unwrap_expr& node) -> ref {
    const auto operand{expr(node.operand)};
    if (id.get_token_type() == token_type_t::BANG) {
        return add_pl(tag::unwrap,
                      writer()
                          .ref_field(operand)
                          .u32_field(static_cast<u32>(unwrap_kind::ASSERT))
                          .body_field({}),
                      id);
    }
    std::vector<inst_id> failure;
    {
        const body_guard guard{*this, failure};
        const auto       residual{add(tag::residual, {}, id)};
        emit_defers(0, residual);
        DISCARD(add_un(tag::ret_residual, residual, 0, id));
    }
    return add_pl(tag::unwrap,
                  writer()
                      .ref_field(operand)
                      .u32_field(static_cast<u32>(unwrap_kind::PROPAGATE))
                      .body_field(failure),
                  id);
}

auto astgen::lower_node(ast::node_id id, const ast::asm_expr& node) -> ref {
    const auto result_type{optional_type(node.result_type)};
    const auto constraint{
        [this](ast::string_handle s) { return ast_.get_as<ast::string_expr>(s).value; }};
    std::vector<std::pair<std::string_view, stdx::option<ref>>> outputs;
    for (const auto& op : node.outputs) {
        outputs.emplace_back(constraint(op.constraint),
                             op.value ? stdx::option<ref>{place(*op.value, use::MUTATE).value}
                                      : stdx::none);
    }
    std::vector<std::pair<std::string_view, stdx::option<ref>>> inputs;
    for (const auto& op : node.inputs) {
        inputs.emplace_back(constraint(op.constraint), optional_expr(op.value));
    }
    u32 options{0};
    for (const auto opt : node.options) { options |= 1U << static_cast<u32>(opt); }

    auto record{writer()};
    record.str_field(constraint(node.tmpl)).ref_field(result_type).count(outputs.size());
    for (const auto& [text, value] : outputs) { record.str_field(text).ref_field(value); }
    record.count(inputs.size());
    for (const auto& [text, value] : inputs) { record.str_field(text).ref_field(value); }
    record.count(node.clobbers.size());
    for (const auto clobber : node.clobbers) { record.str_field(constraint(clobber)); }
    record.u32_field(options);
    return add_pl(tag::asm_expr, record, id);
}

auto astgen::lower_node(ast::node_id id, const ast::cfg_value_expr& node) -> ref {
    if (node.predicate) {
        const auto value{cfg_.resolve_cfg_value(id)};
        const auto boolean{value ? value->as_opt<bool>() : stdx::none};
        if (!boolean) { return to_ref(well_known::undefined); }
        return to_ref(*boolean ? well_known::bool_true : well_known::bool_false);
    }
    const auto chosen{cfg_.chosen_expression(id)};
    if (!chosen) { return to_ref(well_known::undefined); }
    // A selected `@compileError` already fired when the guard was evaluated
    if (const auto call{ast_.get_as_opt<ast::call_expr>(*chosen)};
        call && ast::node_id{call->function}.get_builtin() == syntax::builtin_t::COMPILE_ERROR) {
        return add(tag::unreachable, {}, id);
    }
    return expr(*chosen);
}

auto astgen::lower_node(ast::node_id id, const ast::function_expr& node) -> ref {
    return lower_function(id, node);
}

} // namespace ghoti::uir::detail
