#pragma once

#include <filesystem>
#include <string>

#include <stdx/result.hh>

#include "compiler/module/error.hh"
#include "compiler/module/source_loader.hh"

namespace ghoti::mod {

class file_loader final : public source_loader {
  public:
    // Attempts to obtain the file's source code from disk and load it into memory
    [[nodiscard]] auto load(const std::filesystem::path& path)
        -> stdx::result<std::string, diagnostic> override;

    // Converts the path to its weakly canonical representation
    [[nodiscard]] auto normalize(const std::filesystem::path& path)
        -> stdx::result<std::filesystem::path, error> override;
};

} // namespace ghoti::mod
