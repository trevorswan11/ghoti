#pragma once

#include <filesystem>
#include <iostream>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "driver/clap/error.hh"
#include "driver/cmd/command.hh"

namespace ghoti::cmd {

struct raw_fmt_options {
    std::vector<std::string>  input_paths{};
    bool                      write_in_place{false};
    bool                      check_only{false};
    stdx::option<std::string> stdin_filepath;
    u32                       max_width{100};
    u32                       indent_spaces{4};
};

struct fmt_options {
    std::vector<std::filesystem::path>  input_paths{};
    bool                                write_in_place{false};
    bool                                check_only{false};
    bool                                reading_stdin{false};
    stdx::option<std::filesystem::path> stdin_filepath;
    u32                                 max_width{100};
    u32                                 indent_spaces{4};

    static auto process_raw(const raw_fmt_options& raw, std::ostream& error_stream)
        -> stdx::result<fmt_options, clap::error>;
};

class format final : public command {
  public:
    explicit format(fmt_options opts, std::ostream& error_stream = std::cerr)
        : command{error_stream}, opts_{std::move(opts)} {}

    [[nodiscard]] auto execute() -> stdx::result<void, clap::error> override;

    MAKE_GETTER(opts, const fmt_options&);

  private:
    fmt_options opts_;
};

} // namespace ghoti::cmd
