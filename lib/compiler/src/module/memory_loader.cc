#include "compiler/module/memory_loader.hh"

#include <filesystem>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <stdx/assert.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>

#include "compiler/module/error.hh"

namespace ghoti::mod {

auto memory_loader::add(const std::filesystem::path& path, const std::string& content) -> void {
    PROFILE_FUNCTION();
    const auto normalized{normalize(path)};
    ASSERT(normalized);
    files_[*normalized] = content;
}

auto memory_loader::load(const std::filesystem::path& path)
    -> stdx::result<std::string, diagnostic> {
    PROFILE_FUNCTION();
    auto normalized{normalize(path)};
    if (!normalized) { return make_mod_err(normalized.error()); }
    auto it{files_.find(normalized->string())};
    if (it == files_.end()) {
        return make_mod_err(
            fmt::format("Could not find path '{}' in virtual file system", path.string()),
            error::PATH_DOES_NOT_EXIST);
    }
    return it->second;
}

} // namespace ghoti::mod
