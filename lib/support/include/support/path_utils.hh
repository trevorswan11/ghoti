#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <stdx/option.hh>

namespace ghoti::path_utils {

// Create a relative version of the path if absolute
[[nodiscard]] auto make_relative(const std::filesystem::path& path)
    -> stdx::option<std::filesystem::path>;

// Parses a `file://` URI into a native filesystem path; none for any other scheme
[[nodiscard]] auto uri_to_path(std::string_view uri) -> stdx::option<std::filesystem::path>;

// Renders a filesystem path as a `file://` URI per the LSP spec's URI conventions
[[nodiscard]] auto path_to_uri(const std::filesystem::path& path) -> std::string;

} // namespace ghoti::path_utils
