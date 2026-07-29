#pragma once

#include <filesystem>
#include <iostream>
#include <vector>

#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "driver/clap/error.hh"
#include "driver/cmd/build_options.hh"
#include "driver/cmd/command.hh"

namespace ghoti::cmd {

class build_lib final : public command {
  public:
    explicit build_lib(build_options_base opts, std::ostream& error_stream = std::cerr)
        : command{error_stream}, opts_{std::move(opts)} {}

    build_lib(std::filesystem::path              input_path,
              std::filesystem::path              output_path,
              codegen::target_options            target_opts,
              codegen::optimizer_options         opt_opts,
              std::vector<module_binding>        modules       = {},
              std::vector<std::filesystem::path> extra_objects = {},
              std::ostream&                      error_stream  = std::cerr)
        : command{error_stream}, opts_{.input_path    = std::move(input_path),
                                       .output_path   = std::move(output_path),
                                       .target_opts   = std::move(target_opts),
                                       .opt_opts      = std::move(opt_opts),
                                       .modules       = std::move(modules),
                                       .extra_objects = std::move(extra_objects)} {}

    auto execute() -> stdx::result<void, clap::error> override;

    MAKE_GETTER(opts, const build_options_base&)

  private:
    build_options_base opts_;
};

} // namespace ghoti::cmd
