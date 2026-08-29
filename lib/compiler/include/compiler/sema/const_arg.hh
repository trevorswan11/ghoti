#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <utility>

#include <stdx/hash.hh>
#include <stdx/types.hh>
#include <stdx/variant.hh>

namespace ghoti::sema {

class const_arg {
  public:
    using data_t = stdx::variant<i64, u64, f64, bool, std::string>;

    const_arg() = default;
    explicit const_arg(data_t value) noexcept : data_{std::move(value)} {}

    template <typename T> [[nodiscard]] auto is() const noexcept -> bool { return data_.is<T>(); }
    template <typename T> [[nodiscard]] auto as() const noexcept -> const T& {
        return data_.as<T>();
    }

    [[nodiscard]] auto operator==(const const_arg& other) const noexcept -> bool = default;

    [[nodiscard]] auto hash() const noexcept -> u64 {
        stdx::hasher h{static_cast<u64>(data_.index())};
        data_.visit([&h](const std::string& s) { h.combine<std::string_view>(s); },
                    [&h](f64 v) { h.combine(v); },
                    [&h](const auto& v) { h.combine(static_cast<u64>(v)); });
        return h.finalize();
    }

    // A short, stable spelling for the monomorphized function's mangled name.
    [[nodiscard]] auto mangle() const noexcept -> std::string {
        return data_.visit([](i64 v) { return std::to_string(v); },
                           [](u64 v) { return std::to_string(v); },
                           [](f64 v) { return std::to_string(v); },
                           [](bool v) -> std::string { return v ? "true" : "false"; },
                           [](const std::string& v) {
                               std::string out;
                               out.reserve(v.size());
                               for (const char c : v) {
                                   out +=
                                       (std::isalnum(static_cast<unsigned char>(c)) != 0) ? c : '.';
                               }
                               return out;
                           });
    }

  private:
    data_t data_{i64{0}};
};

} // namespace ghoti::sema
