#include "driver/clap/formatter.hh"

#include <sstream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/color.h>
#include <fmt/ostream.h>
#include <stdx/profiler.hh>
#include <stdx/string.hh>

#include "support/style.hh"

namespace ghoti::clap {

auto formatter::make_subcommands(const CLI::App* app, CLI::AppFormatMode) const -> std::string {
    PROFILE_FUNCTION();
    auto subcmds{app->get_subcommands({})};
    if (subcmds.empty()) { return ""; }

    std::stringstream ss;
    fmt::println(ss, "\n{}:\n", get_label("Commands"));
    for (const auto* sub : subcmds) {
        if (sub->get_disabled_by_default()) { continue; }
        ss << make_subcommand(sub);
    }
    return ss.str();
}

[[nodiscard]] auto formatter::make_group(std::string                     group,
                                         bool                            is_positional,
                                         std::vector<const CLI::Option*> opts) const
    -> std::string {
    PROFILE_FUNCTION();
    std::stringstream ss;

    // The group name is altered to clean up help output
    if (group == "OPTIONS") {
        fmt::print(ss, "\nGeneral Options:\n\n");
    } else {
        fmt::print(ss, "\n{}:\n", group);
    }

    for (const auto* opt : opts) { fmt::print(ss, "{}", make_option(opt, is_positional)); }
    return ss.str();
}

auto formatter::make_help(const CLI::App* app, std::string name, CLI::AppFormatMode mode) const
    -> std::string {
    PROFILE_FUNCTION();
    std::stringstream ss;

    // Trim the second newline (final character) to prevent weird formatting
    const auto usage{make_usage(app, name)};
    ss << fmt::format(style::LIGHT_GREEN_BOLD, "info");
    fmt::print(ss, ": {}", stdx::string::substr(usage, 0, usage.size() - 1));

    fmt::print(ss, "{}", make_subcommands(app, mode));
    fmt::print(ss, "{}", make_groups(app, mode));

    return ss.str();
}

} // namespace ghoti::clap
