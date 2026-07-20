#include "driver/clap/error.hh"

#include <ostream>
#include <string>

#include <fmt/color.h>
#include <stdx/result.hh>

#include "support/style.hh"

namespace ghoti::clap {

auto fatal_error(std::ostream& os, std::string message, error code) -> stdx::err<error> {
    os << fmt::format(style::RED_BOLD, "error: {}\n", message);
    return stdx::err{code};
}

} // namespace ghoti::clap
