#include "compiler/sema/type.hh"

#include <string_view>

#include <gsl/pointers>
#include <gsl/span>
#include <stdx/fixed/enum_map.hh>
#include <stdx/types.hh>

namespace ghoti::sema {

namespace {

using type_mapping = std::pair<type_kind, std::string_view>;

constexpr auto TYPE_KIND_NAMES{stdx::fixed::enum_map<type_kind, std::string_view>::from(
    {},
    type_mapping{type_kind::POISON, "poison"},
    type_mapping{type_kind::I32, "i32"},
    type_mapping{type_kind::I64, "i64"},
    type_mapping{type_kind::ISIZE, "isize"},
    type_mapping{type_kind::U32, "u32"},
    type_mapping{type_kind::U64, "u64"},
    type_mapping{type_kind::USIZE, "usize"},
    type_mapping{type_kind::U8, "u8"},
    type_mapping{type_kind::BOOL, "bool"},
    type_mapping{type_kind::F32, "f32"},
    type_mapping{type_kind::F64, "f64"},
    type_mapping{type_kind::VOID, "void"},
    type_mapping{type_kind::UNDEFINED, "undefined"},
    type_mapping{type_kind::TYPE, "type"},
    type_mapping{type_kind::SLICE, "slice"},
    type_mapping{type_kind::ARRAY, "array"},
    type_mapping{type_kind::POINTER, "pointer"},
    type_mapping{type_kind::REFERENCE, "reference"},
    type_mapping{type_kind::ENUM, "enum"},
    type_mapping{type_kind::STRUCT, "struct"},
    type_mapping{type_kind::UNION, "union"},
    type_mapping{type_kind::FUNCTION, "function"},
    type_mapping{type_kind::LABEL, "label"},
    type_mapping{type_kind::BLOCK, "block"},
    type_mapping{type_kind::MATCH_ARM, "match arm"},
    type_mapping{type_kind::MODULE, "module"},
    type_mapping{type_kind::AUTO, "auto"},
    type_mapping{type_kind::OPAQUE, "opaque"},
    type_mapping{type_kind::NORETURN, "noreturn"})};

} // namespace

auto type_kind_display_name(type_kind kind) noexcept -> std::string_view {
    return TYPE_KIND_NAMES[kind];
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

} // namespace ghoti::sema
