#include "driver/clap/parser.hh"

#include <iostream>
#include <string>
#include <string_view>
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

#include "compiler/codegen/target.hh"
#include "driver/clap/error.hh"
#include "driver/clap/formatter.hh"
#include "driver/cmd/build_exe.hh"
#include "driver/cmd/build_lib.hh"
#include "driver/cmd/build_obj.hh"
#include "driver/cmd/build_options.hh"
#include "driver/cmd/command.hh"
#include "driver/cmd/repl.hh"
#include "ghoti/config.h"

namespace ghoti::clap {

parser::parser(i32 argc, char** argv, std::ostream& error_stream, bool ensure_utf8) noexcept
    : argc_{argc}, error_stream_{error_stream} {
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
    const auto build_exe_cmd{setup_build_exe_subcmd()};
    const auto build_lib_cmd{setup_build_lib_subcmd()};

    // No arguments should be handled by printing help and exiting
    if (argc_ == 1) {
        fmt::println(error_stream_, "{}", app_.help());
        return fatal_error(error_stream_, "expected command argument", error::MISSING_SUBCOMMAND);
    }

    try {
        app_.parse(argc_, argv_);
    } catch (const CLI::ParseError& e) { return stdx::err{static_cast<error>(app_.exit(e))}; };

    if (repl_cmd->parsed()) { return stdx::make_box<cmd::repl>(error_stream_); }

    if (build_obj_cmd->parsed()) {
        auto opts{cmd::build_options::process_raw(
            build_obj_opts_, codegen::output_type::OBJECT, error_stream_)};
        if (!opts) { return stdx::err{opts.error()}; }
        return stdx::make_box<cmd::build_obj>(std::move(*opts), error_stream_);
    }

    if (build_exe_cmd->parsed()) {
        auto opts{cmd::build_options::process_raw(
            build_exe_opts_, codegen::output_type::EXECUTABLE, error_stream_)};
        if (!opts) { return stdx::err{opts.error()}; }
        return stdx::make_box<cmd::build_exe>(std::move(*opts), error_stream_);
    }

    if (build_lib_cmd->parsed()) {
        const auto type{build_lib_opts_.dynamic ? codegen::output_type::DYNAMIC_LIBRARY
                                                : codegen::output_type::STATIC_LIBRARY};
        auto       opts{cmd::build_options::process_raw(build_lib_opts_, type, error_stream_)};
        if (!opts) { return stdx::err{opts.error()}; }
        return stdx::make_box<cmd::build_lib>(std::move(*opts), error_stream_);
    }

    return fatal_error(error_stream_, "expected command argument", error::MISSING_SUBCOMMAND);
}

auto parser::setup_repl_subcmd() -> gsl::not_null<CLI::App*> {
    return app_.add_subcommand("repl", "Run the ghoti REPL");
}

auto parser::setup_build_obj_subcmd() -> gsl::not_null<CLI::App*> {
    auto* build_obj_cmd{app_.add_subcommand("build-obj", "Build an object file from source")};
    build_obj_cmd->add_option("input_file", build_obj_opts_.input, "Input source file (.gh)")
        ->required();
    setup_build_options_flags(build_obj_cmd, build_obj_opts_, "Output object file (.o / .obj)");
    return build_obj_cmd;
}

auto parser::setup_build_exe_subcmd() -> gsl::not_null<CLI::App*> {
    auto* build_exe_cmd{app_.add_subcommand("build-exe", "Build an executable binary from source")};
    build_exe_cmd->add_option("input_file", build_exe_opts_.input, "Input source file (.gh)")
        ->required();
    setup_build_options_flags(build_exe_cmd, build_exe_opts_, "Output executable binary");
    return build_exe_cmd;
}

auto parser::setup_build_lib_subcmd() -> gsl::not_null<CLI::App*> {
    auto* build_lib_cmd{
        app_.add_subcommand("build-lib", "Build a static or dynamic library from source")};
    build_lib_cmd->add_option("input_file", build_lib_opts_.input, "Input source file (.gh)")
        ->required();
    build_lib_cmd->add_flag("--dynamic,--shared",
                            build_lib_opts_.dynamic,
                            "Build a dynamic / shared library instead of a static library");
    setup_build_options_flags(
        build_lib_cmd, build_lib_opts_, "Output library path (.a / .lib / .so / .dylib / .dll)");
    return build_lib_cmd;
}

} // namespace ghoti::clap
