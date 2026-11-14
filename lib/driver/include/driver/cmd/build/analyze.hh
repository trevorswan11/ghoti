#pragma once

#include <iostream>

#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "driver/clap/error.hh"
#include "driver/cmd/build/options.hh"
#include "driver/cmd/command.hh"

namespace ghoti::cmd {

class analyze_cmd final : public command {
  public:
    static constexpr auto KIND{command_kind::ANALYZE};

  public:
    explicit analyze_cmd(build::options opts,
                         bool           as_test      = false,
                         std::ostream&  error_stream = std::cerr)
        : command{error_stream}, opts_{std::move(opts)}, as_test_{as_test} {}

    [[nodiscard]] auto execute() -> stdx::result<void, clap::error> override;
    [[nodiscard]] auto get_kind() const noexcept -> command_kind override { return KIND; }

    MAKE_GETTER(opts, const build::options&)
    [[nodiscard]] auto is_test() const noexcept -> bool { return as_test_; }

  private:
    build::options opts_;
    bool           as_test_;
};

} // namespace ghoti::cmd
