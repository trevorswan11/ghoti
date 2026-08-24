#include "driver/cmd/format/options.hh"

#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <stdx/assert.hh>
#include <stdx/result.hh>
#include <stdx/string.hh>
#include <stdx/utility.hh>

#include "driver/clap/error.hh"
#include "driver/cmd/format/options.hh"

namespace ghoti::cmd::format {

auto options::process_raw(const raw_options& raw, std::ostream& error_stream)
    -> stdx::result<options, clap::error> {
    std::vector<std::filesystem::path> input_paths;
    const auto try_add_file = [&input_paths](const std::filesystem::path& p) {
        if (std::filesystem::is_regular_file(p)) {
            input_paths.emplace_back(p);
        } else if (std::filesystem::is_symlink(p)) {
            input_paths.emplace_back(std::filesystem::read_symlink(p));
        } else {
            return false;
        }
        return true;
    };

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

                const auto& entry_path{entry.path()};
                if (entry_path.has_extension()) {
                    auto ext{entry_path.extension().string()};
                    stdx::string::inplace_lower(ext);
                    if (ext != ".gh") { continue; }
                } else {
                    continue;
                }

                try_add_file(entry_path);
            }
        } else if (!try_add_file(path)) {
            clap::warn_error(error_stream,
                             fmt::format("path '{}' is not a valid file type", path.string()));
            continue;
        }
    }

    const auto reading_stdin{input_paths.empty()};
    if (reading_stdin && raw.write_in_place) {
        return clap::fatal_error(
            error_stream,
            fmt::format("cannot use in-place formatting when reading from stdin"),
            clap::error::CONFLICTING_OPTIONS);
    }

    if (raw.write_in_place && raw.check_only) {
        return clap::fatal_error(
            error_stream,
            fmt::format("cannot use both in-place formatting and error-only checking"),
            clap::error::CONFLICTING_OPTIONS);
    }

    if (raw.max_width < 20 || (raw.indent_spaces < 1 || raw.indent_spaces > 16)) {
        return clap::fatal_error(
            error_stream,
            fmt::format("provided options are not sensible for consistent formatting"),
            clap::error::CONFLICTING_OPTIONS);
    }

    return options{
        .input_paths    = std::move(input_paths),
        .write_in_place = raw.write_in_place,
        .check_only     = raw.check_only,
        .reading_stdin  = reading_stdin,
        .stdin_filepath =
            std::move(raw.stdin_filepath)
                .transform([](const std::string s) -> std::filesystem::path { return s; }),
        .max_width     = raw.max_width,
        .indent_spaces = raw.indent_spaces,
    };
}

} // namespace ghoti::cmd::format
