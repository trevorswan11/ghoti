#pragma once

#include <iostream>
#include <ostream>

#include <CLI/App.hpp>
#include <gsl/pointers>
#include <stdx/memory.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "driver/clap/error.hh"
#include "driver/cmd/build/options.hh"
#include "driver/cmd/command.hh"
#include "driver/cmd/format/options.hh"
#include "driver/platform/win32.hh"

namespace ghoti::clap {

class parser {
  public:
    parser(i32           argc,
           char**        argv,
           std::ostream& error_stream = std::cerr,
           bool          ensure_utf8  = true) noexcept;

    auto parse() -> stdx::result<stdx::box<cmd::command>, error>;

  private:
    [[nodiscard]] auto setup_repl_subcmd() -> gsl::not_null<CLI::App*>;
    [[nodiscard]] auto setup_lsp_subcmd() -> gsl::not_null<CLI::App*>;
    [[nodiscard]] auto setup_build_obj_subcmd() -> gsl::not_null<CLI::App*>;
    [[nodiscard]] auto setup_build_exe_subcmd() -> gsl::not_null<CLI::App*>;
    [[nodiscard]] auto setup_build_lib_subcmd() -> gsl::not_null<CLI::App*>;
    [[nodiscard]] auto setup_fmt_subcmd() -> gsl::not_null<CLI::App*>;

  private:
    i32           argc_;
    char**        argv_;
    std::ostream& error_stream_;

    CLI::App                 app_;
    cmd::build::raw_options  build_obj_opts_;
    cmd::build::raw_options  build_exe_opts_;
    cmd::build::raw_options  build_lib_opts_;
    cmd::format::raw_options fmt_opts_;

#if GHOTI_WINDOWS
    win32::rich_console console_;
#endif
};

} // namespace ghoti::clap
