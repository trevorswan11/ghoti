#pragma once

#include <filesystem>

#include <stdx/option.hh>

namespace ghoti::mod {

// Searches for `std.gh` from the GHOTI_STDLIB environment variable.
// If this path cannot be resolved, searches up to 5 levels up for "lib/std/std.gh"
[[nodiscard]] auto find_stdlib() -> stdx::option<std::filesystem::path>;

// Resolves a path inside the shipped `lib` directory, searched the same way
// as the stdlib: next to an overridden GHOTI_STDLIB first, then up from the executable
[[nodiscard]] auto find_lib_path(const std::filesystem::path& relative)
    -> stdx::option<std::filesystem::path>;

} // namespace ghoti::mod
