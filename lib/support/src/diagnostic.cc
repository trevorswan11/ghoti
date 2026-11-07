#include "support/diagnostic.hh"

#include <ostream>
#include <string>

#include <fmt/color.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <stdx/option.hh>
#include <stdx/utility.hh>

#include "support/style.hh"

namespace ghoti::detail {

auto format_diagnostic(std::ostream&                    os,
                       const formattable_diagnostic&    diag,
                       const stdx::option<std::string>& source_path,
                       stdx::option<bool>               in_terminal) -> std::ostream& {
    const auto tty{in_terminal.value_or(stdx::is_tty())};

    // The source and location play nicely with one another
    if (source_path) {
        const auto& local_style{tty ? style::WHITE_BOLD : style::BASE};
        os << fmt::format(local_style, "{}:", *source_path);
        if (const auto at{diag.location()}) {
            os << fmt::format(local_style, "{}: ", *at);
        } else {
            fmt::print(os, " ");
        }
    }

    // Here the buffer should be "file:loc: " to print level if present
    if (diag.level) {
        const auto name{level_name(*diag.level)};
        os << fmt::format(tty ? level_style(*diag.level) : style::BASE, "{}:", name);
    }

    // The location trails the message when there's no source path to lead with
    fmt::print(os, " {}", diag.message);
    if (const auto at{diag.location()}; !source_path && at) { os << fmt::format(" {}", *at); }
    return os;
}

} // namespace ghoti::detail
