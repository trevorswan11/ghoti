#include "compiler/sema/type.hh"

#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/pointers>
#include <gsl/span>
#include <magic_enum/magic_enum.hpp>
#include <stdx/assert.hh>
#include <stdx/enum.hh>
#include <stdx/fixed/enum_map.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/module/module.hh"
#include "support/int128.hh"
#include "support/string_utils.hh"

namespace ghoti::sema {

namespace {

using type_mapping = std::pair<type_kind, std::string_view>;

// The qualifier a `^`/`&`/`[]` container spells before its element
auto container_qualifier(const type& container) -> std::string_view {
    if (container.is_volatile()) { return container.is_constant() ? "volatile " : "mut volatile "; }
    return container.is_constant() ? "" : "mut ";
}

// A bare `volatile T` / `mut volatile T` type carries its qualifier on its own key
auto leaf_qualifier(const type& leaf) -> std::string_view {
    if (!leaf.is_volatile()) { return ""; }
    return leaf.is_constant() ? "volatile " : "mut volatile ";
}

// `[N]&T` / `[]&T` decays element-wise to `[N]^T` / `[]^T`: same referent, no mutability gain.
[[nodiscard]] auto ref_elem_decays_to_ptr(const type& src_elem, const type& dest_elem) noexcept
    -> bool {
    const auto r{src_elem.get_data().as_opt<types::reference>()};
    const auto p{dest_elem.get_data().as_opt<types::pointer>()};
    if (!r || !p) { return false; }
    if (r->underlying.is_constant() && !p->underlying.is_constant()) { return false; }
    return is_same_unqualified(r->underlying, p->underlying);
}

constexpr auto TYPE_KIND_NAMES{[] {
    stdx::fixed::enum_map<type_kind, string_utils::lowercase_str<32>> map{};
    for (const auto kind : stdx::enum_range<type_kind>()) {
        map[kind] = string_utils::lowercase_str<32>{magic_enum::enum_name(kind)};
    }
    map[type_kind::VOID_]     = string_utils::lowercase_str<32>{"void"};
    map[type_kind::MATCH_ARM] = string_utils::lowercase_str<32>{"match arm"};
    return map;
}()};

} // namespace

auto type_kind_display_name(type_kind kind) noexcept -> std::string_view {
    return TYPE_KIND_NAMES[kind];
}

auto type_kind_display_name(const type& t) -> std::string {
    if (const auto info{as_integer(t)}) {
        return fmt::format("{}{}", info->is_signed ? 'i' : 'u', info->bits);
    }
    return std::string{type_kind_display_name(t.get_kind())};
}

auto as_integer(const type& t) noexcept -> stdx::option<types::integer> {
    if (t.get_kind() != type_kind::INT) { return stdx::none; }
    if (const auto info{t.get_data().as_opt<types::integer>()}) { return *info; }
    // Unresolved pooled twin: recover identity from the key's mirrored fields.
    return types::integer{t.get_key().get_int_bits(), t.get_key().get_int_signed()};
}

auto int_width(const type& t) noexcept -> u16 {
    const auto info{as_integer(t)};
    ASSERT(info, "int_width requires an INT type");
    return info->bits;
}

auto is_i32(const type& t) noexcept -> bool {
    const auto info{as_integer(t)};
    return info && info->bits == 32 && info->is_signed;
}

auto constexpr_int_fits(i128 value, const type& target, u32 ptr_bits) noexcept -> bool {
    u16  bits{0};
    bool is_signed{false};
    switch (target.get_kind()) {
    case type_kind::INT: {
        const auto info{as_integer(target)};
        if (!info) { return true; }
        bits      = info->bits;
        is_signed = info->is_signed;
        break;
    }
    case type_kind::ISIZE:
        bits      = static_cast<u16>(ptr_bits);
        is_signed = true;
        break;
    case type_kind::USIZE: bits = static_cast<u16>(ptr_bits); break;
    default:               return true; // not a concrete integer target
    }
    if (bits == 0) { return false; }
    if (bits >= 128) { return true; }

    if (is_signed) {
        const i128 max{(i128{1} << (bits - 1)) - 1};
        const i128 min{-(i128{1} << (bits - 1))};
        return value >= min && value <= max;
    }
    if (value < 0) { return false; }
    const u128 umax{bits >= 128 ? ~u128{0} : ((u128{1} << bits) - 1)};
    return static_cast<u128>(value) <= umax;
}

auto packed_field_bits(const type& t, u32 ptr_bits) noexcept -> stdx::option<u32> {
    switch (t.get_kind()) {
    case type_kind::INT:       return int_width(t);
    case type_kind::BOOL:      return 1U;
    case type_kind::ISIZE:
    case type_kind::USIZE:
    case type_kind::POINTER:
    case type_kind::REFERENCE: return ptr_bits;
    case type_kind::F16:
    case type_kind::F32:
    case type_kind::F64:
    case type_kind::F80:
    case type_kind::F128:      return float_bits(t.get_kind());
    case type_kind::ENUM:
        if (const auto e{t.get_data().as_opt<types::enum_t>()}) {
            return packed_field_bits(e->underlying, ptr_bits);
        }
        return stdx::none;
    case type_kind::STRUCT:
        if (const auto st{t.get_data().as_opt<types::struct_t>()}; st && st->is_bit_packed()) {
            return packed_backing_bits(*st, ptr_bits);
        }
        return stdx::none;
    case type_kind::UNION:
        if (const auto ut{t.get_data().as_opt<types::union_t>()}; ut && ut->is_bit_packed()) {
            return packed_union_backing_bits(*ut, ptr_bits);
        }
        return stdx::none;
    case type_kind::ARRAY:
        if (const auto arr{t.get_data().as_opt<types::array>()}) {
            if (const auto elem{packed_field_bits(arr->underlying, ptr_bits)}) {
                const u64 total{static_cast<u64>(*elem) * arr->len};
                if (total <= 65'535) { return static_cast<u32>(total); }
            }
        }
        return stdx::none;
    default: return stdx::none;
    }
}

auto packed_backing_bits(const types::struct_t& s, u32 ptr_bits) noexcept -> stdx::option<u32> {
    u64 total{0};
    for (const auto* field : s.fields) {
        if (!field) { continue; }
        const auto bits{packed_field_bits(*field, ptr_bits)};
        if (!bits) { return stdx::none; }
        total += *bits;
    }
    if (total < 1 || total > 65'535) { return stdx::none; }
    return static_cast<u32>(total);
}

auto packed_field_offset(const types::struct_t& s, usize idx, u32 ptr_bits) noexcept -> u32 {
    u64 offset{0};
    for (usize i{0}; i < idx && i < s.fields.size(); ++i) {
        if (s.fields[i] != nullptr) {
            offset += packed_field_bits(*s.fields[i], ptr_bits).value_or(0);
        }
    }
    return static_cast<u32>(offset);
}

auto packed_union_backing_bits(const types::union_t& u, u32 ptr_bits) noexcept
    -> stdx::option<u32> {
    u64 widest{0};
    for (const auto* field : u.fields) {
        if (!field) { continue; }
        const auto bits{packed_field_bits(*field, ptr_bits)};
        if (!bits) { return stdx::none; }
        widest = std::max<u64>(widest, *bits);
    }
    if (widest < 1 || widest > 65'535) { return stdx::none; }
    return static_cast<u32>(widest);
}

auto is_signed_integer(const type& t) noexcept -> bool {
    if (const auto info{as_integer(t)}) { return info->is_signed; }
    return t.get_kind() == type_kind::ISIZE;
}

auto is_unsigned_integer(const type& t) noexcept -> bool {
    if (const auto info{as_integer(t)}) { return !info->is_signed; }
    return t.get_kind() == type_kind::USIZE;
}

auto is_implicit_widenable(const type& from, const type& to) noexcept -> bool {
    const auto from_kind{from.get_kind()};
    const auto to_kind{to.get_kind()};

    // An unsuffixed integer literal coerces to any concrete integer or float
    if (from_kind == type_kind::CONSTEXPR_INT) { return is_numeric(to_kind); }
    if (from_kind == type_kind::CONSTEXPR_FLOAT) {
        return is_float(to_kind) || to_kind == type_kind::CONSTEXPR_FLOAT;
    }
    // A concrete numeric also flows into a `constexpr_*` slot
    if (to_kind == type_kind::CONSTEXPR_INT) { return is_integer(from_kind); }
    if (to_kind == type_kind::CONSTEXPR_FLOAT) {
        return is_float(from_kind) || is_integer(from_kind);
    }

    // A float widens to any wider float (`f16 -> f32 -> f64 -> f80 -> f128`).
    if (is_float(from_kind)) {
        return is_float(to_kind) && float_bits(from_kind) < float_bits(to_kind);
    }

    const auto from_int{as_integer(from)};
    if (!from_int) { return false; }

    // Narrow integers widen into the same-signedness pointer-sized type, mirroring the
    // pre-unification pair table (`i8..i32 -> isize`, `u8..u32 -> usize`).
    if (to_kind == type_kind::ISIZE) { return from_int->is_signed && from_int->bits < 64; }
    if (to_kind == type_kind::USIZE) { return !from_int->is_signed && from_int->bits < 64; }

    const auto to_int{as_integer(to)};
    if (!to_int) { return false; }
    if (to_int->bits <= from_int->bits) { return false; }
    if (from_int->is_signed) { return to_int->is_signed; }
    return true; // uW widens to any wider iV or uV
}

auto type::to_string() const -> std::string {
    // An INT's width lives on the key, so it stringifies even before the payload is resolved.
    if (get_kind() == type_kind::INT) {
        const auto info{as_integer(*this)};
        return fmt::format(
            "{}{}{}", leaf_qualifier(*this), info->is_signed ? 'i' : 'u', info->bits);
    }
    return data_.visit(
        [this](types::pointer ptr) {
            return fmt::format("^{}{}", container_qualifier(*this), ptr.underlying.to_string());
        },
        [this](types::reference ref) {
            return fmt::format("&{}{}", container_qualifier(*this), ref.underlying.to_string());
        },
        [this](types::slice slice) {
            return fmt::format("[{}]{}{}",
                               slice.null_terminated ? ":0" : "",
                               container_qualifier(*this),
                               slice.underlying.to_string());
        },
        [this](types::array arr) {
            return fmt::format("[{}{}]{}{}",
                               arr.len,
                               arr.null_terminated ? ":0" : "",
                               container_qualifier(*this),
                               arr.underlying.to_string());
        },
        [](types::function fn) {
            auto params_str{fmt::to_string(fmt::join(
                fn.params | std::views::transform([](type* param) { return param->to_string(); }),
                ", "))};
            if (fn.is_variadic) { params_str += params_str.empty() ? "..." : ", ..."; }
            return fmt::format("fn({}): {}", params_str, fn.return_type.to_string());
        },
        [](types::closure_t c) { return fmt::format("closure {}", c.signature.to_string()); },
        [](types::module mod) {
            return fmt::format("module {}", mod.imported.path.stem().string());
        },
        [](types::enum_t e) { return fmt::format("enum : {}", e.underlying.to_string()); },
        [this](const auto&) {
            return fmt::format("{}{}", leaf_qualifier(*this), type_kind_display_name(get_kind()));
        });
}

auto type_pool::get_or_emplace(const types::key_t& key) -> gsl::not_null<type*> {
    if (auto it{cache_.find(key)}; it != cache_.end()) { return it->second; }
    auto* t = arena_.make<type>(key).get();
    cache_.emplace(key, t);
    return t;
}

auto type_pool::get_many_unsafe(usize count) noexcept -> gsl::span<type*> {
    return arena_.make_span<type*>(count);
}

auto type_pool::get_many(usize count, type& common_type) noexcept -> gsl::span<type*> {
    auto types{get_many_unsafe(count)};
    for (usize i{0}; i < count; ++i) { types[i] = &common_type; }
    return types;
}

namespace {

auto strip_modifiers(type_pool& pool, const type& old_type, types::mutability_modifiers mut)
    -> gsl::not_null<type*> {
    auto key{old_type.get_key()};
    key.set_mut(key.get_mut() & ~mut);

    // Resolve here since the type information doesn't contain modifier information
    auto new_type{pool[key]};
    new_type->resolve_if<type::data_t>(old_type.get_data());
    if (const auto idx{old_type.get_symbol_table_idx_opt()}) {
        new_type->set_symbol_table_idx(*idx);
    }
    return new_type;
}

} // namespace

auto type_pool::strip_const(const type& t) -> gsl::not_null<type*> {
    if (!t.is_constant()) { return const_cast<type*>(&t); }
    return strip_modifiers(*this, t, types::mut::CONSTANT);
}

auto type_pool::strip_volatile(const type& t) -> gsl::not_null<type*> {
    if (!t.is_volatile()) { return const_cast<type*>(&t); }
    return strip_modifiers(*this, t, types::mut::VOLATILE);
}

auto type_pool::with_const(const type& t, bool is_const) -> gsl::not_null<type*> {
    if (t.is_constant() == is_const) { return const_cast<type*>(&t); }
    if (!is_const) { return strip_modifiers(*this, t, types::mut::CONSTANT); }

    auto key{t.get_key()};
    key.set_mut(key.get_mut() | types::mut::CONSTANT);
    auto new_type{(*this)[key]};
    new_type->resolve_if<type::data_t>(t.get_data());
    if (const auto idx{t.get_symbol_table_idx_opt()}) { new_type->set_symbol_table_idx(*idx); }
    return new_type;
}

auto is_same_unqualified(const type& a, const type& b) noexcept -> bool {
    if (a == b) { return true; }
    if (a.get_kind() != b.get_kind()) { return false; }

    const auto kind{a.get_kind()};
    switch (kind) {
    case type_kind::INT:       return as_integer(a) == as_integer(b);
    case type_kind::BOOL:
    case type_kind::VOID_:
    case type_kind::UNDEFINED:
    case type_kind::NULLPTR:
    case type_kind::TYPE:
    case type_kind::NORETURN:
    case type_kind::OPAQUE:
    case type_kind::POISON:    return true;
    case type_kind::STRUCT:
    case type_kind::UNION:
    case type_kind::ENUM:
    case type_kind::CLOSURE:   return a.get_symbol_table_idx_opt() == b.get_symbol_table_idx_opt();
    case type_kind::DYN:       {
        const auto d_a{a.get_data().as_opt<types::dyn_t>()};
        const auto d_b{b.get_data().as_opt<types::dyn_t>()};
        return d_a && d_b && &d_a->interface == &d_b->interface;
    }
    case type_kind::POINTER: {
        const auto p_a{a.get_data().as_opt<types::pointer>()};
        const auto p_b{b.get_data().as_opt<types::pointer>()};
        if (!p_a || !p_b) { return false; }
        return is_same_unqualified(p_a->underlying, p_b->underlying);
    }
    case type_kind::REFERENCE: {
        const auto r_a{a.get_data().as_opt<types::reference>()};
        const auto r_b{b.get_data().as_opt<types::reference>()};
        if (!r_a || !r_b) { return false; }
        return is_same_unqualified(r_a->underlying, r_b->underlying);
    }
    case type_kind::SLICE: {
        const auto s_a{a.get_data().as_opt<types::slice>()};
        const auto s_b{b.get_data().as_opt<types::slice>()};
        if (!s_a || !s_b) { return false; }
        return s_a->null_terminated == s_b->null_terminated &&
               is_same_unqualified(s_a->underlying, s_b->underlying);
    }
    case type_kind::ARRAY: {
        const auto arr_a{a.get_data().as_opt<types::array>()};
        const auto arr_b{b.get_data().as_opt<types::array>()};
        if (!arr_a || !arr_b) { return false; }
        return arr_a->len == arr_b->len && arr_a->null_terminated == arr_b->null_terminated &&
               is_same_unqualified(arr_a->underlying, arr_b->underlying);
    }
    case type_kind::FUNCTION: {
        const auto f_a{a.get_data().as_opt<types::function>()};
        const auto f_b{b.get_data().as_opt<types::function>()};
        if (!f_a || !f_b) { return a == b; }
        if (f_a->is_variadic != f_b->is_variadic || f_a->params.size() != f_b->params.size()) {
            return false;
        }
        for (const auto& [param_a, param_b] : std::views::zip(f_a->params, f_b->params)) {
            if (!is_same_unqualified(*param_a, *param_b)) { return false; }
        }
        return is_same_unqualified(f_a->return_type, f_b->return_type);
    }
    default: return is_numeric(kind);
    }
}

auto is_assignable(const type& src, const type& dest) noexcept -> bool {
    if (src == dest) { return true; }
    if (src.is_poison() || dest.is_poison()) { return true; }
    if (src.get_kind() == type_kind::UNDEFINED || src.get_kind() == type_kind::NORETURN ||
        src.get_kind() == type_kind::AUTO || dest.get_kind() == type_kind::AUTO) {
        return true;
    }

    // nullptr is assignable to any pointer type, but never to a reference
    if (src.get_kind() == type_kind::NULLPTR) { return dest.get_kind() == type_kind::POINTER; }

    if (is_implicit_widenable(src, dest)) { return true; }
    const auto src_kind{src.get_kind()};
    const auto dest_kind{dest.get_kind()};

    if (const auto ut{dest.get_data().as_opt<types::union_t>()}) {
        if (ut->is_untagged) {
            for (const auto* field : ut->fields) {
                if (is_assignable(src, *field)) { return true; }
            }
        }
    }

    if (src_kind == dest_kind) {
        if (is_numeric(src_kind)) { return is_same_unqualified(src, dest); }
        switch (src_kind) {
        case type_kind::BOOL:
        case type_kind::VOID_:
        case type_kind::STRUCT:
        case type_kind::UNION:
        case type_kind::ENUM:
        case type_kind::TYPE:
        case type_kind::FUNCTION:
        case type_kind::CLOSURE:
            return is_same_unqualified(src, dest);
            // ^S to ^T must be const correct
        case type_kind::POINTER: {
            const auto p_src{src.get_data().as_opt<types::pointer>()};
            const auto p_dest{dest.get_data().as_opt<types::pointer>()};
            if (!p_src || !p_dest) { return false; }
            if (p_src->underlying.is_constant() && !p_dest->underlying.is_constant()) {
                return false;
            }
            if (src.is_constant() && !dest.is_constant()) { return false; }
            // `^T` -> `^dyn I`: the concrete `T`'s conformance is enforced at coercion lowering.
            if (p_dest->underlying.get_kind() == type_kind::DYN) {
                return is_aggregate(p_src->underlying.get_kind());
            }
            return is_same_unqualified(p_src->underlying, p_dest->underlying);
        }
        // &S to &T must be const correct
        case type_kind::REFERENCE: {
            const auto r_src{src.get_data().as_opt<types::reference>()};
            const auto r_dest{dest.get_data().as_opt<types::reference>()};
            if (!r_src || !r_dest) { return false; }
            if (r_src->underlying.is_constant() && !r_dest->underlying.is_constant()) {
                return false;
            }
            if (src.is_constant() && !dest.is_constant()) { return false; }
            if (r_dest->underlying.get_kind() == type_kind::DYN) {
                return is_aggregate(r_src->underlying.get_kind());
            }
            return is_same_unqualified(r_src->underlying, r_dest->underlying);
        }
        // []S to []T must be const and null term correct
        case type_kind::SLICE: {
            const auto s_src{src.get_data().as_opt<types::slice>()};
            const auto s_dest{dest.get_data().as_opt<types::slice>()};
            if (!s_src || !s_dest) { return false; }
            if (s_dest->null_terminated && !s_src->null_terminated) { return false; }
            if (s_src->underlying.is_constant() && !s_dest->underlying.is_constant()) {
                return false;
            }
            if (src.is_constant() && !dest.is_constant()) { return false; }
            return is_same_unqualified(s_src->underlying, s_dest->underlying) ||
                   ref_elem_decays_to_ptr(s_src->underlying, s_dest->underlying);
        }
        case type_kind::ARRAY: {
            const auto a_src{src.get_data().as_opt<types::array>()};
            const auto a_dest{dest.get_data().as_opt<types::array>()};
            if (!a_src || !a_dest) { return false; }
            if (a_src->len != a_dest->len || a_src->null_terminated != a_dest->null_terminated) {
                return false;
            }
            if (a_src->underlying.is_constant() && !a_dest->underlying.is_constant()) {
                return false;
            }
            if (src.is_constant() && !dest.is_constant()) { return false; }
            return is_same_unqualified(a_src->underlying, a_dest->underlying) ||
                   ref_elem_decays_to_ptr(a_src->underlying, a_dest->underlying);
        }
        default: break;
        }
    }

    // Array to Slice coercion: [N]S / [N:0]S -> []T / [:0]T
    if (src_kind == type_kind::ARRAY && dest_kind == type_kind::SLICE) {
        const auto a_src{src.get_data().as_opt<types::array>()};
        const auto s_dest{dest.get_data().as_opt<types::slice>()};
        if (!a_src || !s_dest) { return false; }
        if (s_dest->null_terminated && !a_src->null_terminated) { return false; }
        if (a_src->underlying.is_constant() && !s_dest->underlying.is_constant()) { return false; }
        if (src.is_constant() && !dest.is_constant()) { return false; }
        return is_same_unqualified(a_src->underlying, s_dest->underlying) ||
               ref_elem_decays_to_ptr(a_src->underlying, s_dest->underlying);
    }

    // Function to Function Pointer coercion: fn(...) -> ^fn(...) / ^mut fn(...)
    if (src_kind == type_kind::FUNCTION && dest_kind == type_kind::POINTER) {
        if (const auto p_dest{dest.get_data().as_opt<types::pointer>()}) {
            if (p_dest->underlying.get_kind() == type_kind::FUNCTION) {
                return is_same_unqualified(src, p_dest->underlying);
            }
        }
    }

    return false;
}

} // namespace ghoti::sema
