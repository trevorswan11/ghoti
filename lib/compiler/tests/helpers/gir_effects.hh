#pragma once

#include <string_view>

namespace ghoti::tests::helpers {

// Whether two normalized dumps of one function do the same thing in each segment
[[nodiscard]] auto same_effects(std::string_view lhs, std::string_view rhs) -> bool;

} // namespace ghoti::tests::helpers
