#pragma once

#include <iostream>
#include <ostream>

#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "driver/clap/error.hh"
#include "driver/cmd/command.hh"
#include "driver/cmd/format/options.hh"

namespace ghoti::cmd {

class formatter final : public command {
  public:
    explicit formatter(format::options opts, std::ostream& error_stream = std::cerr)
        : command{error_stream}, opts_{std::move(opts)} {}

    [[nodiscard]] auto execute() -> stdx::result<void, clap::error> override;

    MAKE_GETTER(opts, const format::options&);

  private:
    format::options opts_;
};

} // namespace ghoti::cmd
