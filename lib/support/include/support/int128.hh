#pragma once

#include <string>

namespace ghoti {

using i128 = __int128;
using u128 = unsigned __int128;

[[nodiscard]] auto to_string(u128 v) -> std::string;
[[nodiscard]] auto to_string(i128 v) -> std::string;

} // namespace ghoti
