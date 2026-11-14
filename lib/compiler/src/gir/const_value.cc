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
#include <stdx/assert.hh>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/type.hh"
#include "support/big_int.hh"
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

auto const_value::as_bytes_opt() const -> stdx::option<std::string> {
    if (const auto text{as_opt<std::string>()}) { return *text; }
    const auto arr{as_opt<const_array>()};
    if (!arr) { return stdx::none; }
    std::string out;
    out.reserve(arr->elements.size());
    for (const auto& element : arr->elements) {
        const auto byte{element.as_int_opt()};
        if (!byte) { return stdx::none; }
        out.push_back(static_cast<char>(*byte));
    }
    return out;
}

namespace {

// The struct type a value of `t` lays its fields out by
[[nodiscard]] auto struct_of(stdx::option<sema::type&> t)
    -> stdx::option<const sema::types::struct_t&> {
    if (!t) { return stdx::none; }
    return t->get_data().as_opt<sema::types::struct_t>();
}

[[nodiscard]] auto intern_all(ip::pool& pool, const std::vector<const_value>& items)
    -> stdx::option<std::vector<ip::index>> {
    std::vector<ip::index> out;
    out.reserve(items.size());
    for (const auto& item : items) {
        const auto at{item.intern(pool)};
        if (!at) { return stdx::none; }
        out.emplace_back(*at);
    }
    return out;
}

[[nodiscard]] auto from_all(const ip::pool& pool, const std::vector<ip::index>& items)
    -> std::vector<const_value> {
    std::vector<const_value> out;
    out.reserve(items.size());
    for (const auto at : items) { out.emplace_back(const_value::from_interned(pool, at)); }
    return out;
}

} // namespace

auto const_value::intern(ip::pool& pool) const -> stdx::option<ip::index> {
    using result = stdx::option<ip::index>;
    return data_.visit(
        [&](const stdx::option<sema::type&>& t) -> result {
            if (t) { return t->get_ip_index(); }
            return pool.intern(ip::no_type_value{}, type_);
        },
        [&](const const_array& arr) -> result {
            const auto elements{intern_all(pool, arr.elements)};
            if (!elements) { return stdx::none; }
            return pool.intern(ip::aggregate_value{{elements->begin(), elements->end()}}, type_);
        },
        [&](const const_struct& st) -> result {
            const auto layout{struct_of(type_)};
            if (!layout) { return stdx::none; }
            ip::aggregate_value fields;
            fields.elements.reserve(layout->field_names.size());
            for (const auto name : layout->field_names) {
                const auto field{st.get_field_opt(name)};
                if (!field) {
                    fields.elements.emplace_back(stdx::none);
                    continue;
                }
                const auto at{field->intern(pool)};
                if (!at) { return stdx::none; }
                fields.elements.emplace_back(*at);
            }
            return pool.intern(std::move(fields), type_);
        },
        [&](const const_enum& en) -> result {
            return pool.intern(ip::enum_value{en.name, en.value}, type_);
        },
        [&](const const_union& un) -> result {
            const auto payload{intern_all(pool, un.payload)};
            if (!payload) { return stdx::none; }
            return pool.intern(ip::union_value{un.active_field, *payload}, type_);
        },
        [&](const const_addr& addr) -> result {
            const auto pointee{intern_all(pool, addr.pointee)};
            if (!pointee) { return stdx::none; }
            return pool.intern(ip::address_value{addr.symbol, *pointee}, type_);
        },
        [&](const const_dyn_fat_ptr& dyn) -> result {
            return pool.intern(ip::dyn_value{dyn.data_symbol, dyn.vtable_symbol}, type_);
        },
        [](const const_closure&) -> result { return stdx::none; },
        [](const const_ref&) -> result { return stdx::none; },
        [&](const void_val&) -> result { return pool.intern(ip::void_value{}, type_); },
        [&](const undefined_val&) -> result { return pool.intern(ip::undefined_value{}, type_); },
        [&](const nullptr_val&) -> result { return pool.intern(ip::nullptr_value{}, type_); },
        [&](const poison_val&) -> result { return pool.intern(ip::poison_value{}, type_); },
        [&](const auto& scalar) -> result { return pool.intern(scalar, type_); });
}

auto const_value::from_interned(const ip::pool& pool, ip::index at) -> const_value {
    if (pool.is_type(at)) { return const_value{pool.type_at(at)}; }
    const auto& v{pool.value_at(at)};
    return v.data.visit(
        [&](const ip::no_type_value&) { return const_value{stdx::option<sema::type&>{}, v.type}; },
        [&](const ip::aggregate_value& agg) {
            if (const auto layout{struct_of(v.type)}) {
                const_struct st;
                for (usize i{0}; i < agg.elements.size() && i < layout->field_names.size(); ++i) {
                    if (!agg.elements[i]) { continue; }
                    st.fields.emplace(std::string{layout->field_names[i]},
                                      from_interned(pool, *agg.elements[i]));
                }
                return const_value{std::move(st), v.type};
            }
            const_array arr;
            arr.elements.reserve(agg.elements.size());
            for (const auto e : agg.elements) {
                arr.elements.emplace_back(from_interned(pool, *e));
            }
            return const_value{std::move(arr), v.type};
        },
        [&](const ip::enum_value& en) {
            return const_value{const_enum{en.name, en.value}, v.type};
        },
        [&](const ip::union_value& un) {
            return const_value{const_union{un.active_field, from_all(pool, un.payload)}, v.type};
        },
        [&](const ip::address_value& addr) {
            return const_value{const_addr{addr.symbol, from_all(pool, addr.pointee)}, v.type};
        },
        [&](const ip::dyn_value& dyn) {
            return const_value{const_dyn_fat_ptr{dyn.data_symbol, dyn.vtable_symbol}, v.type};
        },
        [&](const ip::void_value&) { return const_value{void_val{}, v.type}; },
        [&](const ip::undefined_value&) { return const_value{undefined_val{}, v.type}; },
        [&](const ip::nullptr_value&) { return const_value{nullptr_val{}, v.type}; },
        [&](const ip::poison_value&) { return const_value{poison_val{}, v.type}; },
        [](const ip::zero_value&) -> const_value {
            UNREACHABLE("`zero` is a GIR operand, which the evaluator never interns");
        },
        [&](const auto& scalar) { return const_value{scalar, v.type}; });
}

auto const_value::make_string(sema::context& ctx, std::string str) -> const_value {
    auto& t_u8{ctx.get_int(8, false)};
    auto& t_c_str{ctx.get_slice(sema::types::mut::CONSTANT, true, t_u8)};
    return const_value{std::string{str}, t_c_str};
}

auto const_value::to_gir_value(ip::pool& pool) const -> value {
    const auto make{
        [&](ip::value_data payload) { return value::constant(pool, std::move(payload), type_); }};
    return data_.visit(
        [&](const poison_val&) { return make(undefined_val{}); },
        [&](const const_array&) { return make(void_val{}); },
        [&](const const_struct&) { return make(void_val{}); },
        [&](const const_enum& e) { return make(e.value); },
        [&](const const_union&) { return make(void_val{}); },
        [&](const const_closure&) { return make(void_val{}); },
        [&](const const_addr& a) { return make(a.symbol); },
        [&](const const_ref&) { return make(void_val{}); },
        [&](const const_dyn_fat_ptr&) { return make(void_val{}); },
        [&](const f128& f) { return value::constant(pool, f, type_, origin_); },
        [&](const stdx::option<sema::type&>& t) { return value::type_constant(pool, t, type_); },
        [&](const auto& v) { return make(v); });
}

auto const_value::operator==(const const_value& other) const noexcept -> bool {
    if (data_.index() != other.data_.index()) {
        const auto l_int{as_big_int_opt()};
        const auto r_int{other.as_big_int_opt()};
        if (l_int && r_int) { return *l_int == *r_int; }
        return false;
    }
    return data_ == other.data_;
}

auto const_value::identical(const const_value& other) const noexcept -> bool {
    const auto all_identical{
        [](const std::vector<const_value>& l, const std::vector<const_value>& r) {
            return std::ranges::equal(
                l, r, [](const const_value& a, const const_value& b) { return a.identical(b); });
        }};

    if (const auto l{data_.as_opt<f128>()}) {
        const auto r{other.data_.as_opt<f128>()};
        return r && l->bits() == r->bits();
    }
    if (const auto l{data_.as_opt<const_array>()}) {
        const auto r{other.data_.as_opt<const_array>()};
        return r && all_identical(l->elements, r->elements);
    }
    if (const auto l{data_.as_opt<const_union>()}) {
        const auto r{other.data_.as_opt<const_union>()};
        return r && l->active_field == r->active_field && all_identical(l->payload, r->payload);
    }
    if (const auto l{data_.as_opt<const_struct>()}) {
        const auto r{other.data_.as_opt<const_struct>()};
        if (!r || l->fields.size() != r->fields.size()) { return false; }
        return std::ranges::all_of(l->fields, [&](const auto& field) {
            const auto rf{r->get_field_opt(field.first)};
            return rf && field.second.identical(*rf);
        });
    }
    return *this == other;
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
                [&](const big_int& v) {
                    h.combine(v.is_negative() ? 1U : 0U);
                    for (const auto limb : v.magnitude().limbs()) { h.combine(limb); }
                },
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
        [](const big_int& v) { return v.to_string(); },
        [this](f128 v) {
            const auto format{type_ ? sema::float_format_of(*type_) : stdx::none};
            return v.to_string(format.value_or(float_format::QUAD));
        },
        [](bool v) -> std::string { return v ? "true" : "false"; },
        [](const std::string& v) {
            // Any other byte is `.` and its hex value, so two strings never share a name
            std::string out;
            for (const char c : v) {
                const auto byte{static_cast<u8>(c)};
                if (std::isalnum(byte) != 0) {
                    out += c;
                } else {
                    out += fmt::format(".{:02x}", byte);
                }
            }
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
    // `[N]T` and `@Vector(N, T)` hold the same lanes, so only the type changes
    if (value.is<const_array>() && value.get_type() &&
        sema::is_array_vector_pair(*value.get_type(), declared)) {
        value.set_type(declared);
        return value;
    }
    const bool is_number{value.is<i64>() || value.is<u64>() || value.is<i128>() ||
                         value.is<u128>() || value.is<big_int>() || value.is<f128>()};
    if (!is_number || !sema::is_numeric(declared.get_kind())) { return value; }
    const auto format{sema::float_format_of(declared)};
    if (format && !value.is<f128>()) {
        return const_value{*value.int_as_float_opt(*format), declared};
    }
    value.set_type(declared);
    return value;
}

} // namespace ghoti::gir
