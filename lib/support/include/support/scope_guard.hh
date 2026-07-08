#pragma once

#include <utility>

#include <stdx/option.hh>

namespace ghoti {

// An RAII guard that manages pushing/popping onto/from a container/stack
template <typename Container> class scope_guard {
  public:
    template <typename... Args>
    constexpr explicit scope_guard(Container& container, Args&&... args) noexcept
        : container_{container} {
        if constexpr (requires { container_->emplace_back(std::forward<Args>(args)...); }) {
            container_->emplace_back(std::forward<Args>(args)...);
        } else if constexpr (requires { container_->push(std::forward<Args>(args)...); }) {
            container_->push(std::forward<Args>(args)...);
        }
    }

    constexpr ~scope_guard() {
        if (container_) {
            if constexpr (requires { container_->pop_back(); }) {
                container_->pop_back();
            } else if constexpr (requires { container_->pop(); }) {
                container_->pop();
            }
        }
    }

    scope_guard(const scope_guard&)                    = delete;
    auto operator=(const scope_guard&) -> scope_guard& = delete;

    constexpr scope_guard(scope_guard&& other) noexcept
        : container_{std::exchange(other.container_, stdx::none)} {}

    constexpr auto operator=(scope_guard&& other) noexcept -> scope_guard& {
        if (this != &other) {
            if (container_) {
                if constexpr (requires { container_->pop_back(); }) {
                    container_->pop_back();
                } else if constexpr (requires { container_->pop(); }) {
                    container_->pop();
                }
            }
            container_ = std::exchange(other.container_, stdx::none);
        }
        return *this;
    }

  private:
    stdx::option<Container&> container_{stdx::none};
};

} // namespace ghoti
