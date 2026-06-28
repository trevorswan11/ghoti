#include <CLI/CLI.hpp>
#include <catch2/catch_test_macros.hpp>
#include <stdx/memory.hh>

#include "driver/clap/formatter.hh"

namespace ghoti::tests {

TEST_CASE("Formatter provides output") {
    CLI::App app;
    auto     formatter{stdx::make_rc<clap::formatter>()};
    app.formatter(formatter);
    REQUIRE(app.add_subcommand("nothing"));

    CHECK_FALSE(formatter->make_subcommands(&app, CLI::AppFormatMode::All).empty());
    CHECK_FALSE(formatter->make_group("OPTIONS", false, {}).empty());
    CHECK_FALSE(formatter->make_group("", false, {}).empty());
    CHECK_FALSE(formatter->make_help(&app, "name", CLI::AppFormatMode::All).empty());
}

} // namespace ghoti::tests
