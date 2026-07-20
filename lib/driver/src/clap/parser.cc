#include "driver/clap/parser.hh"

#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

#include <CLI/CLI.hpp>
#include <fmt/color.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <gsl/pointers>
#include <stdx/assert.hh>
#include <stdx/config.h>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "driver/clap/error.hh"
#include "driver/clap/formatter.hh"
#include "driver/cmd/build_obj.hh"
#include "driver/cmd/command.hh"
#include "driver/cmd/repl.hh"
#include "ghoti/config.h"

namespace ghoti::clap {

parser::parser(i32 argc, char** argv, std::ostream& os, bool ensure_utf8) noexcept
    : argc_{argc}, os_{os} {
    PROFILE_FUNCTION();
    VERIFY(argc > 0, "The program name must be present");
    app_.formatter(stdx::make_rc<formatter>());
    argv_ = ensure_utf8 ? app_.ensure_utf8(argv) : argv;
}

auto parser::parse() -> stdx::result<stdx::box<cmd::command>, error> {
    PROFILE_FUNCTION();
    app_.usage("Usage: ghoti [command] [options]");
    app_.set_version_flag("-v,--version",
                          fmt::format("ghoti v{} ({})", GHOTI_VERSION_STR, GHOTI_GIT_INFO));
    app_.require_subcommand(1);

    const auto repl_cmd{setup_repl_subcmd()};
    const auto build_obj_cmd{setup_build_obj_subcmd()};

    // No arguments should be handled by printing help and exiting
    if (argc_ == 1) {
        fmt::println(os_, "{}", app_.help());
        return fatal_error(os_, "expected command argument", error::MISSING_SUBCOMMAND);
    }

    try {
        app_.parse(argc_, argv_);
    } catch (const CLI::ParseError& e) { return stdx::err{static_cast<error>(app_.exit(e))}; };

    if (repl_cmd->parsed()) { return stdx::make_box<cmd::repl>(); }

    if (build_obj_cmd->parsed()) {
        codegen::optimizer_options opt_opts;
        opt_opts.debug_logging = build_obj_opts_.debug_passes;
        opt_opts.time_passes   = build_obj_opts_.time_passes;

        if (!build_obj_opts_.opt_level_str.empty()) {
            if (auto level{codegen::parse_opt_level(build_obj_opts_.opt_level_str)}) {
                opt_opts.level = *level;
            } else {
                return fatal_error(
                    os_,
                    fmt::format("invalid optimization level '{}'", build_obj_opts_.opt_level_str),
                    error::INVALID_OPTIMIZATION);
            }
        } else if (build_obj_opts_.release) {
            opt_opts.level = codegen::opt_level::O2;
        } else {
            opt_opts.level = codegen::opt_level::O0;
        }

        std::filesystem::path input_path{build_obj_opts_.input};
        std::filesystem::path output_path;
        if (build_obj_opts_.output.empty()) {
            output_path = input_path;
            output_path.replace_extension(".o");
        } else {
            output_path = build_obj_opts_.output;
        }

        codegen::target_options target_opts{
            .triple_str = build_obj_opts_.target.empty()
                              ? stdx::none
                              : stdx::option<std::string>{build_obj_opts_.target},
            .cpu        = build_obj_opts_.cpu.empty() ? "generic" : build_obj_opts_.cpu,
            .features   = build_obj_opts_.features,
            .level      = opt_opts.level,
        };

        return stdx::make_box<cmd::build_obj>(std::move(input_path),
                                              std::move(output_path),
                                              std::move(target_opts),
                                              std::move(opt_opts));
    }

    return fatal_error(os_, "expected command argument", error::MISSING_SUBCOMMAND);
}

auto parser::setup_repl_subcmd() -> gsl::not_null<CLI::App*> {
    return app_.add_subcommand("repl", "Run the ghoti REPL");
}

auto parser::setup_build_obj_subcmd() -> gsl::not_null<CLI::App*> {
    auto* build_obj_cmd{app_.add_subcommand("build-obj", "Build an object file from source")};
    build_obj_cmd->add_option("input_file", build_obj_opts_.input, "Input source file (.gh)")
        ->required();
    build_obj_cmd->add_option(
        "-o,--output", build_obj_opts_.output, "Output object file (.o / .obj)");
    build_obj_cmd->add_option("--target", build_obj_opts_.target, "Target triple");
    build_obj_cmd->add_option(
        "--cpu", build_obj_opts_.cpu, "Target CPU architecture (default: generic)");
    build_obj_cmd->add_option("--features", build_obj_opts_.features, "Target CPU features");
    build_obj_cmd->add_option(
        "-O,--opt-level", build_obj_opts_.opt_level_str, "Optimization level (0, 1, 2, 3, s, z)");
    build_obj_cmd->add_flag(
        "--release", build_obj_opts_.release, "Build in release mode (defaults to -O2)");
    build_obj_cmd->add_flag("--debug-passes",
                            build_obj_opts_.debug_passes,
                            "Enable debug logging and IR printing after passes");
    build_obj_cmd->add_flag(
        "--time-passes", build_obj_opts_.time_passes, "Enable pass execution timing report");
    return build_obj_cmd;
}

} // namespace ghoti::clap
