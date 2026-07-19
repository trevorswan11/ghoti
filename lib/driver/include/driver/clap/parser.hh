#pragma once

#include <iostream>
#include <ostream>
#include <string>

#include <CLI/App.hpp>
#include <stdx/memory.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/opt_level.hh"
#include "driver/clap/error.hh"
#include "driver/cmd/command.hh"
#include "driver/platform/win32.hh"

namespace ghoti::clap {

class parser {
  public:
    parser(i32 argc, char** argv, std::ostream& os = std::cerr, bool ensure_utf8 = true) noexcept;

    auto parse() -> stdx::result<stdx::box<cmd::command>, error>;
    MAKE_GETTER(opt_options, const codegen::optimizer_options&)

  private:
    [[nodiscard]] auto fatal_error(std::string message, error code) -> stdx::err<error>;

  private:
    i32           argc_;
    char**        argv_;
    std::ostream& os_;

    CLI::App                   app_;
    codegen::optimizer_options opt_options_;
    std::string                opt_level_str_;
    bool                       is_release_{false};

#if GHOTI_WINDOWS
    win32::rich_console console_;
#endif
};

} // namespace ghoti::clap
