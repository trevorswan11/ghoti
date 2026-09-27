#include "driver/cmd/format/options.hh"

#include <filesystem>
#include <ostream>
#include <string>
#include <system_error>
#include <vector>

#include <fmt/format.h>
#include <stdx/result.hh>
#include <stdx/string.hh>
#include <stdx/utility.hh>

#include "driver/clap/error.hh"
#include "driver/cmd/format/options.hh"

namespace ghoti::cmd::format {

namespace {

// Adds `p` if it's a regular file, or the target of `p` if it's a symlink to one
[[nodiscard]] auto try_add_file(std::vector<std::filesystem::path>& input_paths,
                                const std::filesystem::path&        p) -> bool {
    std::error_code ec;
    if (std::filesystem::is_regular_file(p, ec)) {
        input_paths.emplace_back(p);
        return true;
    }

    if (std::filesystem::is_symlink(p, ec) && !ec) {
        auto target{std::filesystem::read_symlink(p, ec)};
        if (!ec) {
            input_paths.emplace_back(std::move(target));
            return true;
        }
    }
    return false;
}

// Recursively collects every `.gh` file under `root`
auto collect_gh_files(const std::filesystem::path&        root,
                      std::vector<std::filesystem::path>& input_paths) -> void {
    std::error_code                               it_ec;
    std::filesystem::recursive_directory_iterator it{root, it_ec};
    if (it_ec) { return; }
    const std::filesystem::recursive_directory_iterator end{};

    while (it != end) {
        std::error_code entry_ec;
        const bool      is_dir{it->is_directory(entry_ec)};
        if (!entry_ec && !is_dir) {
            const auto& entry_path{it->path()};
            if (entry_path.has_extension()) {
                auto ext{entry_path.extension().string()};
                stdx::string::inplace_lower(ext);
                if (ext == ".gh") { DISCARD(try_add_file(input_paths, entry_path)); }
            }
        }

        it.increment(it_ec);
        if (it_ec) { it_ec.clear(); }
    }
}

} // namespace

auto options::process_raw(const raw_options& raw, std::ostream& error_stream)
    -> stdx::result<options, clap::error> {
    std::vector<std::filesystem::path> input_paths;

    for (const auto& input_path : raw.input_paths) {
        std::filesystem::path path{input_path};
        std::error_code       ec;
        if (!std::filesystem::exists(path, ec) || ec) {
            return clap::fatal_error(error_stream,
                                     fmt::format("path '{}' does not exist", path.string()),
                                     clap::error::FILE_NOT_FOUND);
        }

        if (std::filesystem::is_directory(path, ec)) {
            collect_gh_files(path, input_paths);
        } else if (!try_add_file(input_paths, path)) {
            return clap::fatal_error(
                error_stream,
                fmt::format("path '{}' is not a regular file or directory", path.string()),
                clap::error::FILE_NOT_FOUND);
        }
    }

    // Only an empty argument list means stdin; a directory holding no `.gh` files is a no-op
    const auto reading_stdin{raw.input_paths.empty()};
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

    if (raw.max_width < 20) {
        return clap::fatal_error(
            error_stream,
            fmt::format("--max-width must be at least 20; got {}", raw.max_width),
            clap::error::CONFLICTING_OPTIONS);
    }
    if (raw.indent_spaces < 1 || raw.indent_spaces > 16) {
        return clap::fatal_error(
            error_stream,
            fmt::format("--indent-spaces must be between 1 and 16; got {}", raw.indent_spaces),
            clap::error::CONFLICTING_OPTIONS);
    }

    return options{
        .input_paths    = std::move(input_paths),
        .write_in_place = raw.write_in_place,
        .check_only     = raw.check_only,
        .reading_stdin  = reading_stdin,
        .stdin_filepath =
            std::move(raw.stdin_filepath)
                .transform([](const std::string& s) -> std::filesystem::path { return s; }),
        .max_width     = raw.max_width,
        .indent_spaces = raw.indent_spaces,
    };
}

} // namespace ghoti::cmd::format
