#pragma once

#include <string>

#include <stdx/result.hh>
#include <stdx/types.hh>

#include "driver/cmd/command.hh"
#include "driver/clap/error.hh"

namespace ghoti::cmd {

class debug final : public command {
  public:
    auto execute() -> stdx::result<void, clap::error> override;

  private:
    std::string line_;
};

} // namespace ghoti::cmd
