#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <stdx/types.hh>

namespace ghoti::lsp {

inline const std::vector<std::string> DEFAULT_WORKSPACE_EXCLUDES{
    ".git",
    "zig-pkg",
    ".zig-cache",
    "zig-out",
    "editors",
};

inline constexpr usize DEFAULT_WORKSPACE_FILE_CAP{500};

// Recursively finds every `.gh` file under `roots`, skipping any directory named in `excludes`
// and any unreadable subtree
[[nodiscard]] auto
discover_workspace_files(const std::vector<std::filesystem::path>& roots,
                         const std::vector<std::string>& excludes  = DEFAULT_WORKSPACE_EXCLUDES,
                         usize                           max_files = DEFAULT_WORKSPACE_FILE_CAP)
    -> std::vector<std::filesystem::path>;

} // namespace ghoti::lsp
