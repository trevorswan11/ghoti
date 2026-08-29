#include "compiler/gir/const_value.hh"

#include <algorithm>
#include <bit>
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

auto const_value::make_string(sema::context& ctx, std::string str) -> const_value {
    auto& t_u8{ctx.get_builtin_resolved_type(sema::type_kind::U8)};
    auto& t_c_str{ctx.get_slice(sema::types::mut::CONSTANT, true, t_u8)};
    return const_value{std::string{str}, t_c_str};
}

auto const_value::to_gir_value() const noexcept -> value {
    return data_.visit([this](const poison_val&) -> value { return value{undefined_val{}, type_}; },
                       [this](const const_array&) -> value { return value{void_val{}, type_}; },
                       [this](const const_struct&) -> value { return value{void_val{}, type_}; },
                       [this](const const_enum& e) -> value { return value{e.value, type_}; },
                       [this](const const_union&) -> value { return value{void_val{}, type_}; },
                       [this](const const_closure&) -> value { return value{void_val{}, type_}; },
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
                [&](f64 v) { h.combine(std::bit_cast<u64>(v)); },
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
                [&](const auto&) {});
    return h.finalize();
}

auto const_value::mangle() const -> std::string {
    return data_.visit(
        [](i64 v) { return std::to_string(v); },
        [](u64 v) { return std::to_string(v); },
        [](f64 v) { return fmt::format("{}", v); },
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
        [](const auto&) -> std::string { return "v"; });
}

} // namespace ghoti::gir
