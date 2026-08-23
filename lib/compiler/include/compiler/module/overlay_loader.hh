#pragma once

#include <filesystem>
#include <string>

#include <ankerl/unordered_dense.h>
#include <stdx/result.hh>

#include "compiler/module/error.hh"
#include "compiler/module/source_loader.hh"

namespace ghoti::mod {

// Prefers in-memory overlays and falls back to disk otherwise
class overlay_loader final : public source_loader {
  public:
    // Adds/overwrites an overlay for the path, taking priority over its on-disk contents
    [[nodiscard]] auto add(const std::filesystem::path& path, const std::string& content)
        -> stdx::result<void, error>;

    // Drops the overlay for the path, reverting future loads to its on-disk contents
    auto remove(const std::filesystem::path& path) -> void;

    [[nodiscard]] auto load(const std::filesystem::path& path)
        -> stdx::result<std::string, diagnostic> override;

    [[nodiscard]] auto normalize(const std::filesystem::path& path)
        -> stdx::result<std::filesystem::path, error> override;

  private:
    ankerl::unordered_dense::map<std::filesystem::path, std::string> overlays_;
};

} // namespace ghoti::mod
