#pragma once

#include <iostream>

#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "driver/clap/error.hh"
#include "driver/cmd/build/options.hh"
#include "driver/cmd/command.hh"

namespace ghoti::cmd {

class build_obj final : public command {
  public:
    explicit build_obj(build::options opts, std::ostream& error_stream = std::cerr)
        : command{error_stream}, opts_{std::move(opts)} {}

    [[nodiscard]] auto execute() -> stdx::result<void, clap::error> override;

    MAKE_GETTER(opts, const build::options&)

  private:
    build::options opts_;
};

} // namespace ghoti::cmd
