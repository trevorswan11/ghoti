#include "compiler/module/stdlib.hh"

#include <filesystem>
#include <system_error>

#include <stdx/option.hh>
#include <stdx/types.hh>

#include "ghoti/config.h"
#include "support/env.hh"

namespace ghoti::mod {

auto find_stdlib() -> stdx::option<std::filesystem::path> {
    if (const auto env_var{get_env(GHOTI_STDLIB_ENV)}) {
        std::filesystem::path env_path{*env_var};
        std::error_code       ec;
        if (std::filesystem::exists(env_path, ec) && !std::filesystem::is_directory(env_path, ec)) {
            return env_path;
        }
    }

    std::error_code ec;
    auto            cur_dir{std::filesystem::current_path(ec)};
    if (!ec) {
        for (usize depth{0}; depth <= 5; ++depth) {
            auto candidate{cur_dir / "lib" / "std" / "std.gh"};
            if (std::filesystem::exists(candidate, ec) &&
                !std::filesystem::is_directory(candidate, ec)) {
                return candidate;
            }
            if (!cur_dir.has_parent_path() || cur_dir == cur_dir.parent_path()) { break; }
            cur_dir = cur_dir.parent_path();
        }
    }

    return stdx::none;
}

} // namespace ghoti::mod
