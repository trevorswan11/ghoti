#pragma once

#include <cstddef>

#include <gsl/span>
#include <stdx/arena.hh>
#include <stdx/memory.hh>
#include <stdx/types.hh>

namespace ghoti {

constexpr usize ARENA_SIZE{stdx::sizes::kib(64UZ)};
using arena = stdx::arena<ARENA_SIZE>;

template <typename T> auto make_uninit_span(ghoti::arena& arena, usize n) -> gsl::span<T> {
    if (n == 0) { return {}; }
    struct alignas(alignof(T)) raw_slot {
        std::byte bytes[sizeof(T)];
    };
    auto slots{arena.make_span<raw_slot>(n)};
    return gsl::span<T>{reinterpret_cast<T*>(slots.data()), n};
}

} // namespace ghoti
