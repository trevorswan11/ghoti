#pragma once

#include <filesystem>

#include <stdx/option.hh>

namespace ghoti::mod {

// Searches for `std.gh` from the GHOTI_STDLIB environment variable.
// If this path cannot be resolved, searches up to 5 levels up for "lib/std/std.gh"
[[nodiscard]] auto find_stdlib() -> stdx::option<std::filesystem::path>;

} // namespace ghoti::mod
