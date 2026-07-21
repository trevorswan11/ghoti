#pragma once

#include <string>
#include <string_view>

#include <stdx/option.hh>

namespace ghoti {

auto               set_env(const std::string& name, const std::string& value) -> void;
[[nodiscard]] auto get_env(const std::string& name) -> stdx::option<std::string_view>;

} // namespace ghoti
