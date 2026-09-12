#pragma once

#include <stdx/arena.hh>
#include <stdx/memory.hh>
#include <stdx/types.hh>

namespace ghoti {

constexpr usize ARENA_SIZE{stdx::sizes::kib(64UZ)};
using arena = stdx::arena<ARENA_SIZE>;

} // namespace ghoti
