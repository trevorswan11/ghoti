#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <stdx/option.hh>

namespace ghoti {

auto               set_env(const std::string& name, const std::string& value) -> void;
[[nodiscard]] auto get_env(const std::string& name) -> stdx::option<std::string_view>;

// A path from an environment variable made absolute against the working directory
[[nodiscard]] auto get_env_path(const std::string& name) -> stdx::option<std::filesystem::path>;

} // namespace ghoti
