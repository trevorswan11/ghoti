#pragma once

#include <fmt/color.h>

namespace ghoti::style {

constexpr fmt::text_style BASE;
constexpr auto            BOLD{fmt::emphasis::bold};

constexpr auto WHITE{fmt::fg(fmt::color::white)};
constexpr auto WHITE_BOLD{WHITE | BOLD};
constexpr auto RED{fmt::fg(fmt::color::red)};
constexpr auto RED_BOLD{RED | BOLD};
constexpr auto LIGHT_YELLOW{fmt::fg(fmt::color::light_yellow)};
constexpr auto GREEN{fmt::fg(fmt::color::green)};
constexpr auto GREEN_BOLD{GREEN | BOLD};
constexpr auto LIGHT_GREEN{fmt::fg(fmt::color::light_green)};
constexpr auto LIGHT_GREEN_BOLD{LIGHT_GREEN | BOLD};

} // namespace ghoti::style
