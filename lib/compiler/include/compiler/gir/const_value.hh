#pragma once

#include <string>
#include <utility>

#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/sema/type.hh"

namespace ghoti::gir {

struct poison_val {
    [[nodiscard]] constexpr auto operator==(const poison_val&) const noexcept -> bool = default;
};

class const_value {
  public:
    using data_t = stdx::variant<i64,
                                 u64,
                                 f64,
                                 bool,
                                 std::string,
                                 stdx::option<sema::type&>,
                                 void_val,
                                 undefined_val,
                                 poison_val>;

  public:
    constexpr const_value() noexcept = default;
    constexpr explicit const_value(data_t val, stdx::option<sema::type&> t = stdx::none) noexcept
        : data_{std::move(val)}, type_{t} {}

    [[nodiscard]] static constexpr auto make_poison() noexcept -> const_value {
        return const_value{poison_val{}};
    }

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
        return stdx::none;
    }

    [[nodiscard]] constexpr auto as_uint_opt() const noexcept -> stdx::option<u64> {
        if (is<u64>()) { return as<u64>(); }
        if (is<i64>()) {
            if (const auto val{as<i64>()}; val >= 0) { return static_cast<u64>(val); }
        }
        return stdx::none;
    }

    MAKE_GETTER(type, stdx::option<sema::type&>);
    MAKE_DEDUCING_GETTER(data);

    constexpr auto set_type(stdx::option<sema::type&> t) noexcept -> void { type_ = t; }

    [[nodiscard]] auto to_gir_value() const noexcept -> value {
        return data_.visit(
            [this](const poison_val&) -> value { return value{undefined_val{}, type_}; },
            [this](const auto& v) -> value { return value{v, type_}; });
    }

    [[nodiscard]] auto operator==(const const_value& other) const noexcept -> bool = default;

  private:
    data_t                    data_{poison_val{}};
    stdx::option<sema::type&> type_;
};

} // namespace ghoti::gir
