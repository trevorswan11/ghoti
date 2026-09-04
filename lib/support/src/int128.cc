#include "support/int128.hh"

#include <algorithm>
#include <string>

#include <fmt/format.h>

namespace ghoti {

auto to_string(u128 v) -> std::string {
    if (v == 0) { return "0"; }
    std::string digits;
    while (v != 0) {
        digits += static_cast<char>('0' + static_cast<int>(v % 10));
        v /= 10;
    }
    std::ranges::reverse(digits);
    return digits;
}

auto to_string(i128 v) -> std::string {
    // Negate in the unsigned domain so `i128` min does not overflow.
    if (v < 0) { return fmt::format("-{}", to_string(static_cast<u128>(-(v + 1)) + 1)); }
    return to_string(static_cast<u128>(v));
}

} // namespace ghoti
