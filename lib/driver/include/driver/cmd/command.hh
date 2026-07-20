#pragma once

#include <iostream>

#include <stdx/result.hh>
#include <stdx/types.hh>

#include "driver/clap/error.hh"

namespace ghoti::cmd {

class command {
  public:
    explicit command(std::ostream& error_stream = std::cerr) noexcept
        : error_stream_{error_stream} {}
    virtual ~command()                                                      = default;
    [[nodiscard]] virtual auto execute() -> stdx::result<void, clap::error> = 0;

  protected:
    std::ostream& error_stream_;
};

} // namespace ghoti::cmd
