#include "compiler/module/overlay_loader.hh"

#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <system_error>

#include <fmt/format.h>
#include <stdx/assert.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>

#include "compiler/module/error.hh"

namespace ghoti::mod {

auto overlay_loader::add(const std::filesystem::path& path, const std::string& content)
    -> stdx::result<void, error> {
    PROFILE_FUNCTION();
    const auto normalized{TRY(normalize(path))};
    overlays_[normalized] = content;
    return {};
}

auto overlay_loader::remove(const std::filesystem::path& path) -> void {
    PROFILE_FUNCTION();
    if (const auto normalized{normalize(path)}) { overlays_.erase(*normalized); }
}

auto overlay_loader::load(const std::filesystem::path& path)
    -> stdx::result<std::string, diagnostic> {
    PROFILE_FUNCTION();
    auto normalized{normalize(path)};
    if (!normalized) { return make_mod_err(normalized.error()); }
    if (auto it{overlays_.find(*normalized)}; it != overlays_.end()) { return it->second; }
    const auto& resolved{*normalized};

    std::error_code ec;
    if (!std::filesystem::exists(resolved, ec)) {
        return make_mod_err(fmt::format("Path '{}' does not exist", resolved.string()),
                            error::PATH_DOES_NOT_EXIST);
    }

    if (!std::filesystem::is_regular_file(resolved, ec)) {
        return make_mod_err(fmt::format("Path '{}' is not a file", resolved.string()),
                            error::PATH_IS_NOT_FILE);
    }

    std::ifstream file{resolved, std::ios::in};
    if (!file.is_open()) {
        return make_mod_err(fmt::format("Failed to open file at path: '{}'", resolved.string()),
                            error::FAILED_TO_OPEN_FILE);
    }
    std::ostringstream buf;
    buf << file.rdbuf();
    return buf.str();
}

// Overlay keys must match the same canonicalization disk reads use, so both agree on identity
auto overlay_loader::normalize(const std::filesystem::path& path)
    -> stdx::result<std::filesystem::path, error> {
    PROFILE_FUNCTION();
    std::error_code ec;
    auto            canonical_path{std::filesystem::weakly_canonical(path, ec)};
    if (ec) { return stdx::err{error::NORMALIZATION_FAILED}; }
    return canonical_path;
}

} // namespace ghoti::mod
