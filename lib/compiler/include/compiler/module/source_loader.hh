#pragma once

#include <filesystem>
#include <string>

#include <stdx/result.hh>

#include "compiler/module/error.hh"

namespace ghoti::mod {

class source_loader {
  public:
    virtual ~source_loader() = default;
    [[nodiscard]] virtual auto load(const std::filesystem::path& path)
        -> stdx::result<std::string, diagnostic> = 0;

    // Normalizes the path to behave as required by the loader
    [[nodiscard]] virtual auto normalize(const std::filesystem::path& path)
        -> stdx::result<std::filesystem::path, error> = 0;
};

} // namespace ghoti::mod
