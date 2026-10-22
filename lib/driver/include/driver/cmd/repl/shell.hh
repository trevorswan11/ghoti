#pragma once

#include <string>

#include <stdx/result.hh>
#include <stdx/types.hh>

#include "driver/clap/error.hh"
#include "driver/cmd/command.hh"

namespace ghoti::cmd {

class shell final : public command {
  public:
    static constexpr auto KIND{command_kind::SHELL};

  public:
    using command::command;
    [[nodiscard]] auto execute() -> stdx::result<void, clap::error> override;
    [[nodiscard]] auto get_kind() const noexcept -> command_kind override { return KIND; }

  private:
    std::string line_;
};

} // namespace ghoti::cmd
