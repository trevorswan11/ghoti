#pragma once

#include <filesystem>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>

#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/utility.hh>

#include "driver/clap/error.hh"
#include "driver/cmd/command.hh"
#include "driver/cmd/format/options.hh"

namespace ghoti::cmd {

class formatter final : public command {
  public:
    explicit formatter(format::options opts,
                       std::ostream&   error_stream = std::cerr,
                       std::ostream&   out_stream   = std::cout)
        : command{error_stream}, out_stream_{out_stream}, opts_{std::move(opts)} {}

    [[nodiscard]] auto execute() -> stdx::result<void, clap::error> override;

    MAKE_GETTER(opts, const format::options&);

  private:
    [[nodiscard]] auto
    process_target(std::string_view                           source_code,
                   const stdx::option<std::string>&           display_name,
                   stdx::option<const std::filesystem::path&> file_path = stdx::none) -> bool;

  private:
    std::ostream&   out_stream_;
    format::options opts_;
};

} // namespace ghoti::cmd
