#include "driver/clap/parser.hh"

#include <iostream>
#include <string>

#include <CLI/CLI.hpp>
#include <fmt/color.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <stdx/assert.hh>
#include <stdx/config.h>
#include <stdx/memory.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/codegen/opt_level.hh"
#include "driver/clap/error.hh"
#include "driver/clap/formatter.hh"
#include "driver/cmd/command.hh"
#include "driver/cmd/repl.hh"
#include "ghoti/config.h"
#include "support/style.hh"

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

    app_.add_option("-O,--opt-level", opt_level_str_, "Optimization level (0, 1, 2, 3, s, z)");
    app_.add_flag("--release", is_release_, "Build in release mode (defaults to -O2)");
    app_.add_flag("--debug-passes",
                  opt_options_.debug_logging,
                  "Enable debug logging and IR printing after passes");
    app_.add_flag("--time-passes", opt_options_.time_passes, "Enable pass execution timing report");

    const auto* repl_cmd{app_.add_subcommand("repl", "Run the ghoti REPL")};

    // No arguments should be handled by printing help an exiting
    if (argc_ == 1) {
        fmt::println(os_, "{}", app_.help());
        return fatal_error("expected command argument", error::MISSING_SUBCOMMAND);
    }

    try {
        app_.parse(argc_, argv_);
    } catch (const CLI::ParseError& e) { return stdx::err{static_cast<error>(app_.exit(e))}; };

    if (!opt_level_str_.empty()) {
        if (auto level{codegen::parse_opt_level(opt_level_str_)}) {
            opt_options_.level = *level;
        } else {
            return fatal_error(fmt::format("invalid optimization level '{}'", opt_level_str_),
                               error::INVALID_OPTIMIZATION);
        }
    } else if (is_release_) {
        opt_options_.level = codegen::opt_level::O2;
    } else {
        opt_options_.level = codegen::opt_level::O0;
    }

    if (repl_cmd->parsed()) { return stdx::make_box<cmd::repl>(); }
    return stdx::err{error::INVALID_OPTIMIZATION};
}

auto parser::fatal_error(std::string message, error code) -> stdx::err<error> {
    os_ << fmt::format(style::RED_BOLD, "error: {}\n", message);
    return stdx::err{code};
}

} // namespace ghoti::clap
