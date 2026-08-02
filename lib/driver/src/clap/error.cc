#include "driver/clap/error.hh"

#include <ostream>
#include <string>

#include <fmt/color.h>
#include <fmt/ostream.h>
#include <stdx/result.hh>

#include "support/diagnostic.hh"

namespace ghoti::clap {

auto fatal_error(std::ostream& os, std::string message, error code) -> stdx::err<error> {
    os << fmt::format(detail::level_style(diagnostic_level::ERROR), "error: ");
    fmt::println(os, "{}", message);
    return stdx::err{code};
}

auto warn_error(std::ostream& os, std::string message) -> void {
    os << fmt::format(detail::level_style(diagnostic_level::WARNING), "warning: ");
    fmt::println(os, "{}", message);
}

} // namespace ghoti::clap
