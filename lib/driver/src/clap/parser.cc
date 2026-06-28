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
    if (ast_app->parsed()) { parsed_.emplace<cmd::debug>(); }

    return {};
}

} // namespace ghoti::clap
