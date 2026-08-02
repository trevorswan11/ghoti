#include "driver/cmd/format.hh"

#include <filesystem>
#include <ostream>
#include <vector>

#include <fmt/format.h>
#include <stdx/assert.hh>
#include <stdx/result.hh>
#include <stdx/string.hh>
#include <stdx/utility.hh>

#include "driver/clap/error.hh"

namespace ghoti::cmd {

auto fmt_options::process_raw(const raw_fmt_options& raw, std::ostream& error_stream)
    -> stdx::result<fmt_options, clap::error> {
    std::vector<std::filesystem::path> input_paths;
    for (const auto& input_path : raw.input_paths) {
        std::filesystem::path path{input_path};
        if (!std::filesystem::exists(path)) {
            clap::warn_error(error_stream, fmt::format("path '{}' does not exist", path.string()));
            continue;
        }

        if (std::filesystem::is_directory(path)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator{path}) {
                ASSERT(entry.exists(), "Recursive walker encountered non-existent file");
                if (entry.is_directory()) { continue; }
                if (entry.path().has_extension()) {
                    auto ext{entry.path().extension().string()};
                    stdx::string::inplace_lower(ext);
                    if (ext != ".gh") { continue; }
                } else {
                    continue;
                }

                if (std::filesystem::is_regular_file(path)) {
                    input_paths.emplace_back(std::move(path));
                } else if (std::filesystem::is_symlink(path)) {
                    input_paths.emplace_back(std::filesystem::read_symlink(path));
                }
            }
        } else if (std::filesystem::is_regular_file(path)) {
            input_paths.emplace_back(std::move(path));
        } else if (std::filesystem::is_symlink(path)) {
            input_paths.emplace_back(std::filesystem::read_symlink(path));
        } else {
            clap::warn_error(error_stream,
                             fmt::format("path '{}' is not a valid file type", path.string()));
            continue;
        }
    }

    if (input_paths.empty() && !raw.stdin_filepath) {
        return clap::fatal_error(
            error_stream,
            fmt::format("formatter requires at least one file when not reading from stdin"),
            clap::error::NO_FILES_TO_FORMAT);
    }

    return fmt_options{
        .input_paths    = std::move(input_paths),
        .write_in_place = raw.write_in_place,
        .check_only     = raw.check_only,
        .stdin_filepath = std::move(raw.stdin_filepath),
        .max_width      = raw.max_width,
        .indent_spaces  = raw.indent_spaces,
    };
}

auto format::execute() -> stdx::result<void, clap::error> { TODO(); }

} // namespace ghoti::cmd
