#include "support/env.hh"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <stdx/option.hh>

#include "ghoti/config.h"

#if GHOTI_WINDOWS
#    include <sec_api/stdlib_s.h>
#else
#    include <stdlib.h>
#endif

namespace ghoti {

auto set_env(const std::string& name, const std::string& value) -> void {
#if GHOTI_WINDOWS
    ::_putenv_s(name.c_str(), value.c_str());
#else
    ::setenv(name.c_str(), value.c_str(), 1);
#endif
}

auto get_env(const std::string& name) -> stdx::option<std::string_view> {
    const auto* var{std::getenv(name.c_str())};
    if (!var) { return stdx::none; }
    return var;
}

namespace {

auto trim_env_value(std::string_view value) -> std::string_view {
    constexpr std::string_view whitespace{" \t\r\n"};
    const auto                 first{value.find_first_not_of(whitespace)};
    if (first == std::string_view::npos) { return {}; }
    value = value.substr(first, value.find_last_not_of(whitespace) - first + 1);

    const auto quoted{value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
                      value.back() == value.front()};
    return quoted ? value.substr(1, value.size() - 2) : value;
}

} // namespace

auto get_env_path(const std::string& name) -> stdx::option<std::filesystem::path> {
    const auto value{get_env(name)};
    if (!value) { return stdx::none; }
    const auto trimmed{trim_env_value(*value)};
    if (trimmed.empty()) { return stdx::none; }

    std::filesystem::path path{trimmed};
    std::error_code       ec;
    if (auto absolute{std::filesystem::absolute(path, ec)}; !ec) { path = std::move(absolute); }
    return path.lexically_normal();
}

} // namespace ghoti
