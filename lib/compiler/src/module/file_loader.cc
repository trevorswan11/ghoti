#include "compiler/module/file_loader.hh"

#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <system_error>

#include <fmt/format.h>
#include <stdx/profiler.hh>
#include <stdx/result.hh>

#include "compiler/module/error.hh"

namespace ghoti::mod {

auto file_loader::load(const std::filesystem::path& path) -> stdx::result<std::string, diagnostic> {
    PROFILE_FUNCTION();
    if (!std::filesystem::exists(path)) {
        return make_mod_err(fmt::format("Path '{}' does not exist", path.string()),
                            error::PATH_DOES_NOT_EXIST);
    }

    if (!std::filesystem::is_regular_file(path)) {
        return make_mod_err(fmt::format("Path '{}' is not a file", path.string()),
                            error::PATH_IS_NOT_FILE);
    }

    std::ifstream file{path, std::ios::in};
    if (!file.is_open()) {
        return make_mod_err(fmt::format("Failed to open file at path: '{}'", path.string()),
                            error::FAILED_TO_OPEN_FILE);
    }
    std::stringstream buf;
    buf << file.rdbuf();
    return buf.str();
}

auto file_loader::normalize(const std::filesystem::path& path)
    -> stdx::result<std::filesystem::path, error> {
    PROFILE_FUNCTION();
    std::error_code ec;
    auto            canonical_path{std::filesystem::weakly_canonical(path, ec)};
    if (ec) { return stdx::err{error::NORMALIZATION_FAILED}; }
    return canonical_path;
}

} // namespace ghoti::mod
