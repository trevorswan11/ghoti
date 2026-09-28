#include "compiler/module/stdlib.hh"

#include <filesystem>

#include <stdx/option.hh>
#include <stdx/types.hh>

#include "ghoti/config.h"
#include "support/env.hh"
#include "support/path_utils.hh"
#include "support/subprocess.hh"

namespace ghoti::mod {

auto find_stdlib() -> stdx::option<std::filesystem::path> {
    if (const auto env_var{get_env(GHOTI_STDLIB_ENV)}) {
        std::filesystem::path env_path{*env_var};
        if (path_utils::is_file(env_path)) { return env_path; }
    }

    const auto found{find_lib_path("std/std.gh")};
    if (found && path_utils::is_file(*found)) { return found; }
    return stdx::none;
}

auto find_lib_path(const std::filesystem::path& relative) -> stdx::option<std::filesystem::path> {
    // An overridden stdlib sits at `<lib>/std/std.gh`, so its `lib` holds the other resources too
    if (const auto env_var{get_env(GHOTI_STDLIB_ENV)}) {
        const std::filesystem::path env_path{*env_var};
        if (path_utils::is_file(env_path)) {
            auto candidate{env_path.parent_path().parent_path() / relative};
            if (path_utils::exists(candidate)) { return candidate; }
        }
    }

    const auto self{self_exe_path()};
    auto       cur_dir{self.parent_path()};
    for (i32 depth{0}; depth <= GHOTI_STDLIB_MAX_SEARCH_DEPTH; ++depth) {
        auto candidate{cur_dir / "lib" / relative};
        if (path_utils::exists(candidate)) { return candidate; }
        if (!cur_dir.has_parent_path() || cur_dir == cur_dir.parent_path()) { break; }
        cur_dir = cur_dir.parent_path();
    }

    return stdx::none;
}

} // namespace ghoti::mod
