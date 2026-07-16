#include "driver/clap/parser.hh"

#include <iostream>

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
#include <string>

#include "compiler/codegen/opt_level.hh"
#include "driver/clap/formatter.hh"
#include "driver/cmd/debug.hh"
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

auto parser::parse() -> stdx::result<void, i32> {
    PROFILE_FUNCTION();
    app_.usage("Usage: ghoti [command] [options]");
    app_.set_version_flag("-v,--version",
                          fmt::format("ghoti v{} ({})", GHOTI_VERSION_STR, GHOTI_GIT_INFO));
    std::string opt_level_str;
    app_.add_option("-O,--opt-level", opt_level_str, "Optimization level (0, 1, 2, 3, s, z)");
    app_.add_flag("--release", is_release_, "Build in release mode (defaults to -O2)");
    app_.add_flag("--debug-passes",
                  opt_options_.debug_logging,
                  "Enable debug logging and IR printing after passes");
    app_.add_flag("--time-passes", opt_options_.time_passes, "Enable pass execution timing report");
    app_.require_subcommand(1);

    const auto* ast_app{app_.add_subcommand("debug", "Run the CLI interactive debugger")};

    // No arguments should be handled by printing help an exiting
    if (argc_ == 1) {
        fmt::println(os_, "{}", app_.help());
        os_ << fmt::format(style::RED_BOLD, "error");
        fmt::println(os_, ": expected command argument");
        return stdx::err{1};
    }

    try {
        app_.parse(argc_, argv_);
    } catch (const CLI::ParseError& e) { return stdx::err{app_.exit(e)}; };

    if (!opt_level_str.empty()) {
        if (auto level{codegen::parse_opt_level(opt_level_str)}) {
            opt_options_.level = *level;
        } else {
            os_ << fmt::format(style::RED_BOLD, "error");
            fmt::println(os_, ": invalid optimization level '{}'", opt_level_str);
            return stdx::err{1};
        }
    } else if (is_release_) {
        opt_options_.level = codegen::opt_level::O2;
    } else {
        opt_options_.level = codegen::opt_level::O0;
    }

    if (ast_app->parsed()) { parsed_.emplace<cmd::debug>(); }

    return {};
}

} // namespace ghoti::clap
