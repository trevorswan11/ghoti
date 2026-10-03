#include "compiler/gir/const_value.hh"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/pointers>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/type.hh"
#include "support/float128.hh"
#include "support/int128.hh"

namespace ghoti::gir {

auto const_array::operator==(const const_array& other) const noexcept -> bool {
    return elements == other.elements;
}

auto const_struct::get_field_opt(std::string_view name) const noexcept
    -> stdx::option<const const_value&> {
    if (auto it{fields.find(name)}; it != fields.end()) { return it->second; }
    return stdx::none;
}

auto const_struct::operator==(const const_struct& other) const noexcept -> bool {
    return fields == other.fields;
}

auto const_union::operator==(const const_union& other) const noexcept -> bool {
    return active_field == other.active_field && payload == other.payload;
}

auto const_closure::operator==(const const_closure& other) const noexcept -> bool {
    return fn_node.get_index() == other.fn_node.get_index() && module == other.module &&
           captures == other.captures;
}

auto const_addr::operator==(const const_addr& other) const noexcept -> bool {
    return symbol == other.symbol && pointee == other.pointee;
}

auto const_value::make_string(sema::context& ctx, std::string str) -> const_value {
    auto& t_u8{ctx.get_int(8, false)};
    auto& t_c_str{ctx.get_slice(sema::types::mut::CONSTANT, true, t_u8)};
    return const_value{std::string{str}, t_c_str};
}

auto const_value::to_gir_value() const noexcept -> value {
    return data_.visit(
        [this](const poison_val&) -> value { return value{undefined_val{}, type_}; },
        [this](const const_array&) -> value { return value{void_val{}, type_}; },
        [this](const const_struct&) -> value { return value{void_val{}, type_}; },
        [this](const const_enum& e) -> value { return value{e.value, type_}; },
        [this](const const_union&) -> value { return value{void_val{}, type_}; },
        [this](const const_closure&) -> value { return value{void_val{}, type_}; },
        [this](const const_addr& a) -> value { return value{a.symbol, type_}; },
        [this](const const_ref&) -> value { return value{void_val{}, type_}; },
        [this](const const_dyn_fat_ptr&) -> value { return value{void_val{}, type_}; },
        [this](const f128& f) -> value { return value{f, type_, origin_}; },
        [this](const auto& v) -> value { return value{v, type_}; });
}

auto const_value::operator==(const const_value& other) const noexcept -> bool {
    if (data_.index() != other.data_.index()) {
        const auto l_int{as_int_opt()};
        const auto r_int{other.as_int_opt()};
        if (l_int && r_int) { return *l_int == *r_int; }
        return false;
    }
    return data_ == other.data_;
}

auto const_value::hash() const noexcept -> u64 {
    // Match operator==: any int-like value hashes by its integer regardless of variant arm.
    if (const auto i{as_int_opt()}) {
        stdx::hasher h{1};
        h.combine(static_cast<u64>(*i));
        return h.finalize();
    }

    stdx::hasher h{static_cast<u64>(data_.index()) + 2};
    data_.visit([&](const std::string& s) { h.combine<std::string_view>(s); },
                [&](f128 v) {
                    // `-0 == +0`, so both zeros hash alike
                    const auto bits{v.is_zero() ? u128{} : v.bits()};
                    h.combine(bits.low);
                    h.combine(bits.high);
                },
                [&](bool v) { h.combine(v ? 1U : 0U); },
                [&](const stdx::option<sema::type&>& t) {
                    h.combine(t ? reinterpret_cast<u64>(t.get()) : 0U);
                },
                [&](const const_array& a) {
                    for (const auto& e : a.elements) { h.combine(e.hash()); }
                },
                [&](const const_struct& s) {
                    u64 acc{0};
                    for (const auto& [k, v] : s.fields) {
                        stdx::hasher fh{stdx::hash<std::string_view>{}(k)};
                        fh.combine(v.hash());
                        acc ^= fh.finalize(); // order-independent
                    }
                    h.combine(acc);
                },
                [&](const const_union& u) {
                    h.combine<std::string_view>(u.active_field);
                    for (const auto& e : u.payload) { h.combine(e.hash()); }
                },
                [&](const const_closure& c) {
                    h.combine(static_cast<u64>(c.fn_node.get_index()));
                    h.combine(reinterpret_cast<u64>(c.module.get()));
                    for (const auto& [k, v] : c.captures.fields) {
                        stdx::hasher fh{stdx::hash<std::string_view>{}(k)};
                        fh.combine(v.hash());
                        h.combine(fh.finalize());
                    }
                },
                [&](const const_addr& a) { h.combine<std::string_view>(a.symbol); },
                [&](const const_ref& r) {
                    h.combine(r.frame);
                    h.combine<std::string_view>(r.binding);
                    for (const auto& step : r.path) {
                        h.combine<std::string_view>(step.name);
                        h.combine(static_cast<u64>(step.index));
                    }
                },
                [&](const const_dyn_fat_ptr& d) {
                    h.combine<std::string_view>(d.data_symbol);
                    h.combine<std::string_view>(d.vtable_symbol);
                },
                [&](const auto&) {});
    return h.finalize();
}

auto const_value::mangle() const -> std::string {
    return data_.visit(
        [](i64 v) { return fmt::to_string(v); },
        [](u64 v) { return fmt::to_string(v); },
        [](i128 v) { return fmt::to_string(v); },
        [](u128 v) { return fmt::to_string(v); },
        [this](f128 v) {
            const auto format{type_ ? sema::float_format_of(*type_) : stdx::none};
            return v.to_string(format.value_or(float_format::QUAD));
        },
        [](bool v) -> std::string { return v ? "true" : "false"; },
        [](const std::string& v) {
            std::string out;
            for (const char c : v) { out += (std::isalnum(static_cast<u8>(c)) != 0) ? c : '.'; }
            return out;
        },
        [](const stdx::option<sema::type&>& t) -> std::string {
            return t ? t->to_string() : "type";
        },
        [](const const_enum& e) { return fmt::format("{}.{}", e.name, e.value); },
        [](const const_array& a) {
            std::vector<std::string> parts;
            for (const auto& e : a.elements) { parts.emplace_back(e.mangle()); }
            return fmt::format("arr.{}", fmt::join(parts, "."));
        },
        [](const const_struct& s) {
            using ordered_field = std::pair<std::string_view, gsl::not_null<const const_value*>>;
            std::vector<ordered_field> ordered;
            for (const auto& [k, v] : s.fields) { ordered.emplace_back(k, &v); }
            std::ranges::sort(ordered, {}, &ordered_field::first);

            std::vector<std::string> parts;
            for (const auto& [k, v] : ordered) {
                parts.emplace_back(fmt::format("{}.{}", k, v->mangle()));
            }
            return fmt::format("s.{}", fmt::join(parts, "."));
        },
        [](const const_union& u) {
            return fmt::format("u.{}.{}",
                               u.active_field,
                               u.payload.empty() ? std::string{} : u.payload.front().mangle());
        },
        [](const const_closure& c) {
            std::vector<std::string> parts;
            for (const auto& [k, v] : c.captures.fields) {
                parts.emplace_back(fmt::format("{}.{}", k, v.mangle()));
            }
            std::ranges::sort(parts);
            return fmt::format("cl.{}.{}", c.fn_node.get_index(), fmt::join(parts, "."));
        },
        [](const const_addr& a) { return fmt::format("addr.{}", a.symbol); },
        [](const const_ref& r) { return fmt::format("ref.{}.{}", r.frame, r.binding); },
        [](const const_dyn_fat_ptr& d) {
            return fmt::format("dyn.{}.{}", d.data_symbol, d.vtable_symbol);
        },
        [](const auto&) -> std::string { return "v"; });
}

auto with_declared_type(const_value value, sema::type& declared) -> const_value {
    const bool is_number{value.is<i64>() || value.is<u64>() || value.is<i128>() ||
                         value.is<u128>() || value.is<f128>()};
    if (!is_number || !sema::is_numeric(declared.get_kind())) { return value; }
    const auto format{sema::float_format_of(declared)};
    if (format && !value.is<f128>()) {
        return const_value{*value.int_as_float_opt(*format), declared};
    }
    value.set_type(declared);
    return value;
}

} // namespace ghoti::gir
