#include "driver/cmd/build/options.hh"

#include <filesystem>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <gsl/pointers>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/string.hh>

#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "compiler/module/module.hh"
#include "compiler/module/stdlib.hh"
#include "compiler/sema/analyzer.hh"
#include "driver/clap/error.hh"
#include "ghoti/config.h"
#include "support/path_utils.hh"

namespace ghoti::cmd::build {

auto options::process_raw(const raw_options&   raw,
                          codegen::output_type type,
                          std::ostream&        error_stream) -> stdx::result<options, clap::error> {
    codegen::optimizer_options opt_opts{
        .debug_logging = raw.debug_passes,
        .time_passes   = raw.time_passes,
    };

    if (!raw.opt_level_str.empty()) {
        if (auto level{codegen::parse_opt_level(raw.opt_level_str)}) {
            opt_opts.level = *level;
        } else {
            return clap::fatal_error(
                error_stream,
                fmt::format("invalid optimization level '{}'", raw.opt_level_str),
                clap::error::INVALID_OPTIMIZATION);
        }
    } else if (raw.release) {
        opt_opts.level = codegen::opt_level::O2;
    } else {
        opt_opts.level = codegen::opt_level::O0;
    }

    std::filesystem::path input_path{raw.input};
    std::filesystem::path output_path;
    if (raw.output.empty()) {
        output_path = input_path;
        const auto default_ext{codegen::get_default_output_extension(
            type, raw.target.empty() ? stdx::none : stdx::option<std::string_view>{raw.target})};
        if (default_ext.empty()) {
            output_path.replace_extension("");
        } else {
            output_path.replace_extension(default_ext);
        }
    } else {
        output_path = raw.output;
    }

    codegen::target_options target_opts{
        .triple_str = raw.target.empty() ? stdx::none : stdx::option<std::string>{raw.target},
        .cpu        = raw.cpu.empty() ? "generic" : raw.cpu,
        .features   = raw.features,
        .level      = opt_opts.level,
    };

    std::vector<module_binding> modules;
    for (const auto& raw_mod : raw.module_raw_args) {
        const auto comma_pos{raw_mod.find(',')};
        if (comma_pos == std::string::npos || comma_pos == 0 || comma_pos + 1 >= raw_mod.size()) {
            return clap::fatal_error(
                error_stream,
                fmt::format("invalid module format '{}', expected '<name>,<path>'", raw_mod),
                clap::error::INVALID_MODULE_SPEC);
        }
        std::string name{stdx::string::substr(raw_mod, 0, comma_pos)};
        std::string mod_path{stdx::string::substr(raw_mod, comma_pos + 1)};
        modules.emplace_back(std::move(name), std::filesystem::path{std::move(mod_path)});
    }

    std::vector<std::filesystem::path> extra_objects;
    extra_objects.reserve(raw.extra_objects.size());
    for (const auto& obj : raw.extra_objects) { extra_objects.emplace_back(obj); }

    std::vector<std::filesystem::path> library_paths;
    library_paths.reserve(raw.library_paths.size());
    for (const auto& dir : raw.library_paths) { library_paths.emplace_back(dir); }

    return options{
        .input_path    = std::move(input_path),
        .output_path   = std::move(output_path),
        .target_opts   = std::move(target_opts),
        .opt_opts      = std::move(opt_opts),
        .modules       = std::move(modules),
        .extra_objects = std::move(extra_objects),
        .library_paths = std::move(library_paths),
        .libraries     = raw.libraries,
        .dynamic       = raw.dynamic,
    };
}

auto options::make_path_relative() -> void {
    if (auto rel{path_utils::make_relative(input_path)}) { input_path = std::move(*rel); }
}

auto options::setup_module_manager(mod::module_manager& manager, std::ostream& error_stream)
    -> stdx::result<void, clap::error> {
    if (const auto stdlib_path{mod::find_stdlib()}) {
        if (auto res{manager.add_library_module("std", *stdlib_path)}; !res) {
            return clap::fatal_error(
                error_stream,
                fmt::format("failed to register stdlib: {}",
                            res.error().get_message().value_or(GHOTI_UNKNOWN_ERROR)),
                clap::error::COMPILATION_FAILED);
        }
    } else {
        // Otherwise this just defers to a confusing "Unknown module 'std'" on first import.
        return clap::fatal_error(
            error_stream,
            fmt::format("could not locate the ghoti standard library; set {} to its std.gh "
                        "path, or run from within a source checkout",
                        GHOTI_STDLIB_ENV),
            clap::error::COMPILATION_FAILED);
    }

    for (const auto& mod : modules) {
        std::error_code ec;
        if (!std::filesystem::exists(mod.path, ec)) {
            return clap::fatal_error(
                error_stream,
                fmt::format("module '{}' root path '{}' not found", mod.name, mod.path.string()),
                clap::error::FILE_NOT_FOUND);
        }

        if (auto res{manager.add_library_module(mod.name, mod.path)}; !res) {
            return clap::fatal_error(
                error_stream,
                fmt::format("failed to register module '{}': {}",
                            mod.name,
                            res.error().get_message().value_or(GHOTI_UNKNOWN_ERROR)),
                clap::error::COMPILATION_FAILED);
        }
    }

    return {};
}

auto options::analyze(sema::analyzer&      analyzer,
                      mod::module_manager& manager,
                      std::ostream&        error_stream)
    -> stdx::result<gsl::not_null<ghoti::mod::module*>, clap::error> {
    if (!analyzer.analyze(input_path)) { return stdx::err{clap::error::COMPILATION_FAILED}; }

    auto module_result{manager.try_get_file_module(input_path)};
    if (!module_result) {
        return clap::fatal_error(error_stream,
                                 fmt::format("failed to retrieve module '{}'", input_path.string()),
                                 clap::error::COMPILATION_FAILED);
    }

    auto module{*module_result};
    if (module->is_poisoned() || module->is_errored()) {
        return stdx::err{clap::error::COMPILATION_FAILED};
    }
    return module;
}

auto setup_flags(CLI::App* subcmd, raw_options& opts, std::string_view output_desc) -> void {
    subcmd->add_option("-o,--output", opts.output, std::string{output_desc});
    subcmd->add_option(
        "-m,--module", opts.module_raw_args, "Register a library module (format: <name>,<path>)");
    subcmd->add_option("--object", opts.extra_objects, "Link precompiled object file or library");
    subcmd->add_option(
        "-L,--library-path", opts.library_paths, "Add directory to library search paths");
    subcmd->add_option("-l,--library", opts.libraries, "Link against library name");
    subcmd->add_option("--target", opts.target, "Target triple");
    subcmd->add_option("--cpu", opts.cpu, "Target CPU architecture (default: generic)");
    subcmd->add_option("--features", opts.features, "Target CPU features");
    subcmd->add_option(
        "-O,--opt-level", opts.opt_level_str, "Optimization level (0, 1, 2, 3, s, z)");
    subcmd->add_flag("--release", opts.release, "Build in release mode (defaults to -O2)");
    subcmd->add_flag(
        "--debug-passes", opts.debug_passes, "Enable debug logging and IR printing after passes");
    subcmd->add_flag("--time-passes", opts.time_passes, "Enable pass execution timing report");
}

} // namespace ghoti::cmd::build
