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
#include "driver/cmd/build/executable.hh"
#include "driver/cmd/build/library.hh"
#include "driver/cmd/build/object.hh"
#include "driver/cmd/build/options.hh"
#include "driver/cmd/command.hh"
#include "driver/cmd/format/formatter.hh"
#include "driver/cmd/format/options.hh"
#include "driver/cmd/lsp/server.hh"
#include "driver/cmd/repl/shell.hh"
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
    const auto lsp_cmd{setup_lsp_subcmd()};
    const auto build_obj_cmd{setup_build_obj_subcmd()};
    const auto build_exe_cmd{setup_build_exe_subcmd()};
    const auto build_lib_cmd{setup_build_lib_subcmd()};
    const auto fmt_cmd{setup_fmt_subcmd()};

    // No arguments should be handled by printing help and exiting
    if (argc_ == 1) {
        fmt::println(error_stream_, "{}", app_.help());
        return fatal_error(error_stream_, "expected command argument", error::MISSING_SUBCOMMAND);
    }

    try {
        app_.parse(argc_, argv_);
    } catch (const CLI::ParseError& e) { return stdx::err{static_cast<error>(app_.exit(e))}; };

    if (repl_cmd->parsed()) { return stdx::make_box<cmd::shell>(error_stream_); }
    if (lsp_cmd->parsed()) { return stdx::make_box<cmd::lsp_server>(error_stream_); }

    if (build_obj_cmd->parsed()) {
        auto opts{TRY(cmd::build::options::process_raw(
            build_obj_opts_, codegen::output_type::OBJECT, error_stream_))};
        return stdx::make_box<cmd::build_obj>(std::move(opts), error_stream_);
    }

    if (build_exe_cmd->parsed()) {
        auto opts{TRY(cmd::build::options::process_raw(
            build_exe_opts_, codegen::output_type::EXECUTABLE, error_stream_))};
        return stdx::make_box<cmd::build_exe>(std::move(opts), error_stream_);
    }

    if (build_lib_cmd->parsed()) {
        const auto type{build_lib_opts_.dynamic ? codegen::output_type::DYNAMIC_LIBRARY
                                                : codegen::output_type::STATIC_LIBRARY};
        auto opts{TRY(cmd::build::options::process_raw(build_lib_opts_, type, error_stream_))};
        return stdx::make_box<cmd::build_lib>(std::move(opts), error_stream_);
    }

    if (fmt_cmd->parsed()) {
        auto opts{TRY(cmd::format::options::process_raw(fmt_opts_, error_stream_))};
        return stdx::make_box<cmd::formatter>(std::move(opts), error_stream_);
    }

    return fatal_error(error_stream_, "expected command argument", error::MISSING_SUBCOMMAND);
}

auto parser::setup_repl_subcmd() -> gsl::not_null<CLI::App*> {
    return app_.add_subcommand("repl", "Run the ghoti REPL");
}

auto parser::setup_lsp_subcmd() -> gsl::not_null<CLI::App*> {
    return app_.add_subcommand("lsp", "Run the ghoti language server over stdio");
}

auto parser::setup_build_obj_subcmd() -> gsl::not_null<CLI::App*> {
    auto* sub{app_.add_subcommand("build-obj", "Build an object file from source")};
    sub->add_option("input_file", build_obj_opts_.input, "Input source file (.gh)")->required();
    cmd::build::setup_flags(sub, build_obj_opts_, "Output object file (.o / .obj)");
    return sub;
}

auto parser::setup_build_exe_subcmd() -> gsl::not_null<CLI::App*> {
    auto* sub{app_.add_subcommand("build-exe", "Build an executable binary from source")};
    sub->add_option("input_file", build_exe_opts_.input, "Input source file (.gh)")->required();
    cmd::build::setup_flags(sub, build_exe_opts_, "Output executable binary");
    return sub;
}

auto parser::setup_build_lib_subcmd() -> gsl::not_null<CLI::App*> {
    auto* sub{app_.add_subcommand("build-lib", "Build a static or dynamic library from source")};
    sub->add_option("input_file", build_lib_opts_.input, "Input source file (.gh)")->required();
    sub->add_flag("--dynamic,--shared",
                  build_lib_opts_.dynamic,
                  "Build a dynamic / shared library instead of a static library");
    cmd::build::setup_flags(
        sub, build_lib_opts_, "Output library path (.a / .lib / .so / .dylib / .dll)");
    return sub;
}

auto parser::setup_fmt_subcmd() -> gsl::not_null<CLI::App*> {
    auto* sub{app_.add_subcommand("fmt", "Format ghoti source file and directories")};

    sub->add_option("files", fmt_opts_.input_paths, "Files or directories to format recursively");
    sub->add_flag("-w,--write", fmt_opts_.write_in_place, "Overwrite target files in-place");
    sub->add_flag("--check", fmt_opts_.check_only, "Checks formatting and errors on discrepancy");
    sub->add_option("--stdin,--stdin-filepath",
                    fmt_opts_.stdin_filepath,
                    "Virtual file path when reading from stdin");
    sub->add_option(
        "-m,--max-width", fmt_opts_.max_width, "Maximum column line width (default: 100)");
    sub->add_option("-i,--indent-spaces",
                    fmt_opts_.indent_spaces,
                    "Number of spaces to use for indenting (default: 4)");
    return sub;
}

} // namespace ghoti::clap
