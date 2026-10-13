#pragma once

#include <stdx/types.hh>

#include "support/int128.hh"

namespace ghoti::sema {

// The values a range with constant bounds takes: `count` of them from `first`, moving by `step`.
struct range_steps {
    i128 first;
    i128 count;
    i128 step;

    [[nodiscard]] constexpr auto at(i128 k) const noexcept -> i128 { return first + (step * k); }
    [[nodiscard]] constexpr auto last() const noexcept -> i128 { return at(count - 1); }
    [[nodiscard]] constexpr auto descends() const noexcept -> bool { return step < 0; }
};

[[nodiscard]] constexpr auto range_steps_of(i128 lo, i128 hi, bool inclusive) noexcept
    -> range_steps {
    const bool down{hi < lo};
    const i128 span{down ? lo - hi : hi - lo};
    return range_steps{.first = lo, .count = span + (inclusive ? 1 : 0), .step = down ? -1 : 1};
}

} // namespace ghoti::sema
