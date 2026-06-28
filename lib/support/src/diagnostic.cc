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
        if (diag.location) {
            os << fmt::format(local_style, "{}: ", *diag.location);
        } else {
            fmt::print(os, " ");
        }
    }

    // Here the buffer should be "file:loc: " to print level if present
    if (diag.level) {
        const auto name{level_name(*diag.level)};
        os << fmt::format(tty ? level_style(*diag.level) : style::BASE, "{}:", name);
    }

    // The optional message changes position based on source path presence
    if (diag.message) {
        fmt::print(os, " {}", *diag.message);
    } else {
        fmt::print(os, " {}", diag.error_name);
    }
    if (!source_path && diag.location) { os << fmt::format(" {}", *diag.location); }
    return os;
}

} // namespace ghoti::detail
