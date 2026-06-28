#pragma once

#include <concepts>

#include <stdx/type_traits.hh>
#include <stdx/types.hh>

namespace ghoti {

template <typename T>
concept Countable = std::integral<T> && !std::same_as<T, bool>;

// A simple counter that with RAII-based up/down counting
template <Countable Underlying> class counter {
  public:
    class guard {
      public:
        constexpr explicit guard(counter& c) : c_{&c} { c_->increment(); }
        ~guard() {
            if (c_) { c_->decrement(); }
        }

        guard(const guard&)                    = delete;
        auto operator=(const guard&) -> guard& = delete;

        guard(guard&& other) noexcept : c_{std::exchange(other.c_, nullptr)} {}
        auto operator=(const guard&&) -> guard& = delete;

      private:
        counter* c_;
    };

  public:
    constexpr auto increment() noexcept -> void { count_ += static_cast<Underlying>(1); }
    constexpr auto decrement() noexcept -> void { count_ -= static_cast<Underlying>(1); }

    constexpr operator bool() noexcept { return count_ != static_cast<Underlying>(0); }
    constexpr operator Underlying() noexcept { return count_; }

    constexpr auto               operator<=>(const counter&) const noexcept        = default;
    [[nodiscard]] constexpr auto operator==(const counter&) const noexcept -> bool = default;

    template <std::convertible_to<Underlying> T>
    constexpr auto operator<=>(const T& other) const noexcept {
        return count_ <=> static_cast<Underlying>(other);
    }

    template <std::convertible_to<Underlying> T>
    [[nodiscard]] constexpr auto operator==(const T& other) const noexcept -> bool {
        return count_ == static_cast<Underlying>(other);
    }

  private:
    Underlying count_{static_cast<Underlying>(0)};
};

using default_counter = counter<usize>;

} // namespace ghoti
