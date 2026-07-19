#pragma once

#include <stdx/result.hh>
#include <stdx/types.hh>

#include "driver/clap/error.hh"

namespace ghoti::cmd {

class command {
  public:
    virtual ~command()                                = default;
    [[nodiscard]] virtual auto execute() -> stdx::result<void, clap::error> = 0;
};

} // namespace ghoti::cmd
