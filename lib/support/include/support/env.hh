#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <stdx/option.hh>

namespace ghoti {

auto               set_env(const std::string& name, const std::string& value) -> void;
[[nodiscard]] auto get_env(const std::string& name) -> stdx::option<std::string_view>;

// A path from an environment variable, made absolute against the working directory.
// Surrounding whitespace and one pair of matching quotes are dropped, since cmd.exe's
// `set VAR="..."` keeps the quotes in the value. None when unset or empty
[[nodiscard]] auto get_env_path(const std::string& name) -> stdx::option<std::filesystem::path>;

} // namespace ghoti
