#pragma once

#include <iostream>

#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "driver/clap/error.hh"
#include "driver/cmd/build_options.hh"
#include "driver/cmd/command.hh"

namespace ghoti::cmd {

class build_lib final : public command {
  public:
    explicit build_lib(build_options opts, std::ostream& error_stream = std::cerr)
        : command{error_stream}, opts_{std::move(opts)} {}

    [[nodiscard]] auto execute() -> stdx::result<void, clap::error> override;

    MAKE_GETTER(opts, const build_options&)

  private:
    build_options opts_;
};

} // namespace ghoti::cmd
