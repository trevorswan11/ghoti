#pragma once

#include <iostream>
#include <ostream>

#include <CLI/App.hpp>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/variant.hh>

#include "driver/cmd/debug.hh"
#include "driver/platform/win32.hh"

namespace ghoti::clap {

using parsed_t = stdx::variant<stdx::monostate, cmd::debug>;

class parser {
  public:
    parser(i32 argc, char** argv, std::ostream& os = std::cerr, bool ensure_utf8 = true) noexcept;

    auto               parse() -> stdx::result<void, i32>;
    [[nodiscard]] auto get_parsed() noexcept -> parsed_t& { return parsed_; }

  private:
    i32           argc_;
    char**        argv_;
    std::ostream& os_;

    CLI::App app_;
    parsed_t parsed_;

#if GHOTI_WINDOWS
    win32::rich_console console_;
#endif
};

} // namespace ghoti::clap
