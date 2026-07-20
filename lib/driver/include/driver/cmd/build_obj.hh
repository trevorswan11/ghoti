#pragma once

#include <filesystem>
#include <string>

#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "driver/clap/error.hh"
#include "driver/cmd/command.hh"

namespace ghoti::cmd {

struct build_obj_opts {
    std::string input;
    std::string output;
    std::string target;
    std::string cpu{"generic"};
    std::string features;
    std::string opt_level_str;
    bool        release{false};
    bool        debug_passes{false};
    bool        time_passes{false};
};

class build_obj final : public command {
  public:
    build_obj(std::filesystem::path      input_path,
              std::filesystem::path      output_path,
              codegen::target_options    target_opts,
              codegen::optimizer_options opt_opts)
        : input_path_{std::move(input_path)}, output_path_{std::move(output_path)},
          target_opts_{std::move(target_opts)}, opt_opts_{std::move(opt_opts)} {}

    auto execute() -> stdx::result<void, clap::error> override;

    MAKE_GETTER(input_path, const std::filesystem::path&)
    MAKE_GETTER(output_path, const std::filesystem::path&)
    MAKE_GETTER(target_opts, const codegen::target_options&)
    MAKE_GETTER(opt_opts, const codegen::optimizer_options&)

  private:
    std::filesystem::path      input_path_;
    std::filesystem::path      output_path_;
    codegen::target_options    target_opts_;
    codegen::optimizer_options opt_opts_;
};

} // namespace ghoti::cmd
