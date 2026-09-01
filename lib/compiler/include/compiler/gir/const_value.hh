#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "compiler/ast/id.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/sema/type.hh"

namespace ghoti::sema { struct context; } // namespace ghoti::sema
namespace ghoti::mod { struct module; }   // namespace ghoti::mod

namespace ghoti::gir {

class const_value;

struct poison_val {
    [[nodiscard]] constexpr auto operator==(const poison_val&) const noexcept -> bool = default;
};

struct const_array {
    std::vector<const_value> elements;
    [[nodiscard]] auto       operator==(const const_array& other) const noexcept -> bool;
};

struct const_struct {
    using fields_map_t = ankerl::unordered_dense::
        map<std::string, const_value, stdx::string_transparent_hash, stdx::string_transparent_eq>;

    fields_map_t       fields;
    [[nodiscard]] auto get_field_opt(std::string_view name) const noexcept
        -> stdx::option<const const_value&>;
    [[nodiscard]] auto operator==(const const_struct& other) const noexcept -> bool;
};

struct const_enum {
    std::string        name;
    i64                value{0};
    [[nodiscard]] auto operator==(const const_enum&) const noexcept -> bool = default;
};

struct const_union {
    std::string              active_field;
    std::vector<const_value> payload;
    [[nodiscard]] auto       operator==(const const_union& other) const noexcept -> bool;
};

struct const_closure {
    ast::node_id               fn_node;
    stdx::option<mod::module&> module{};
    const_struct               captures;
    [[nodiscard]] auto         operator==(const const_closure& other) const noexcept -> bool;
};

class const_value {
  public:
    using data_t = stdx::variant<i64,
                                 u64,
                                 f64,
                                 bool,
                                 std::string,
                                 stdx::option<sema::type&>,
                                 const_array,
                                 const_struct,
                                 const_enum,
                                 const_union,
                                 const_closure,
                                 void_val,
                                 undefined_val,
                                 nullptr_val,
                                 poison_val>;

  public:
    constexpr const_value() noexcept = default;
    constexpr explicit const_value(sema::type& t) noexcept
        : data_{stdx::option<sema::type&>{t}}, type_{t} {}
    constexpr explicit const_value(data_t val, stdx::option<sema::type&> t = stdx::none) noexcept
        : data_{std::move(val)}, type_{t} {}

    [[nodiscard]] static constexpr auto make_poison() noexcept -> const_value {
        return const_value{poison_val{}};
    }

    [[nodiscard]] static auto make_string(sema::context& ctx, std::string str) -> const_value;

    template <typename T> [[nodiscard]] constexpr auto is() const noexcept -> bool {
        return data_.is<T>();
    }

    template <typename T, typename Self>
    [[nodiscard]] constexpr auto as(this Self&& self) noexcept -> decltype(auto) {
        return std::forward<Self>(self).data_.template as<T>();
    }

    template <typename T>
    [[nodiscard]] constexpr auto as_opt(this auto& self) noexcept -> decltype(auto) {
        return self.data_.template as_opt<T>();
    }

    [[nodiscard]] constexpr auto is_poison() const noexcept -> bool { return is<poison_val>(); }

    [[nodiscard]] constexpr auto as_int_opt() const noexcept -> stdx::option<i64> {
        if (is<i64>()) { return as<i64>(); }
        if (is<u64>()) { return static_cast<i64>(as<u64>()); }
        if (is<const_enum>()) { return as<const_enum>().value; }
        return stdx::none;
    }

    [[nodiscard]] constexpr auto as_uint_opt() const noexcept -> stdx::option<u64> {
        if (is<u64>()) { return as<u64>(); }
        if (is<i64>()) {
            if (const auto val{as<i64>()}; val >= 0) { return static_cast<u64>(val); }
        }
        if (const auto en{as_opt<const_enum>()}) {
            if (en->value >= 0) { return static_cast<u64>(en->value); }
        }
        return stdx::none;
    }

    MAKE_GETTER(type, stdx::option<sema::type&>);
    MAKE_DEDUCING_GETTER(data);

    constexpr auto     set_type(stdx::option<sema::type&> t) noexcept -> void { type_ = t; }
    [[nodiscard]] auto to_gir_value() const noexcept -> value;
    [[nodiscard]] auto operator==(const const_value& other) const noexcept -> bool;

    [[nodiscard]] auto hash() const noexcept -> u64;
    [[nodiscard]] auto mangle() const -> std::string;

  private:
    data_t                    data_{poison_val{}};
    stdx::option<sema::type&> type_;
};

} // namespace ghoti::gir
