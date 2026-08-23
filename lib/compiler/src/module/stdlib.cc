#include "compiler/module/stdlib.hh"

#include <filesystem>
#include <system_error>

#include <stdx/option.hh>
#include <stdx/types.hh>

#include "ghoti/config.h"
#include "support/env.hh"
#include "support/subprocess.hh"

namespace ghoti::mod {

auto find_stdlib() -> stdx::option<std::filesystem::path> {
    if (const auto env_var{get_env(GHOTI_STDLIB_ENV)}) {
        std::filesystem::path env_path{*env_var};
        std::error_code       ec;
        if (std::filesystem::exists(env_path, ec) && !std::filesystem::is_directory(env_path, ec)) {
            return env_path;
        }
    }

    const auto self{self_exe_path()};
    auto       cur_dir{self.parent_path()};
    for (i32 depth{0}; depth <= GHOTI_STDLIB_MAX_SEARCH_DEPTH; ++depth) {
        auto candidate{cur_dir / "lib" / "std" / "std.gh"};
        if (std::filesystem::exists(candidate) && !std::filesystem::is_directory(candidate)) {
            return candidate;
        }
        if (!cur_dir.has_parent_path() || cur_dir == cur_dir.parent_path()) { break; }
        cur_dir = cur_dir.parent_path();
    }

    return stdx::none;
}

} // namespace ghoti::mod
