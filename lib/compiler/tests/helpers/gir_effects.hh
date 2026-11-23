#pragma once

#include <string_view>

namespace ghoti::tests::helpers {

// Whether two normalized dumps of one function do the same thing in each segment: the same
// stores, calls and jumps in the same order, over the same values. Loads of memory nothing but
// an initializer writes, address arithmetic and pure operators may be computed anywhere, and a
// load of a scratch aggregate built from constants reads the constant it was given
[[nodiscard]] auto same_effects(std::string_view lhs, std::string_view rhs) -> bool;

} // namespace ghoti::tests::helpers
