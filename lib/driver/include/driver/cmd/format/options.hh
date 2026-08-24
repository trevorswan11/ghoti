#pragma once

#include <filesystem>
#include <iostream>
#include <ostream>
#include <string>
#include <vector>

#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "driver/clap/error.hh"

namespace ghoti::cmd::format {

struct raw_options {
    std::vector<std::string>  input_paths{};
    bool                      write_in_place{false};
    bool                      check_only{false};
    stdx::option<std::string> stdin_filepath;
    u32                       max_width{100};
    u32                       indent_spaces{4};
};

struct options {
    std::vector<std::filesystem::path>  input_paths{};
    bool                                write_in_place{false};
    bool                                check_only{false};
    bool                                reading_stdin{false};
    stdx::option<std::filesystem::path> stdin_filepath;
    u32                                 max_width{100};
    u32                                 indent_spaces{4};

    static auto process_raw(const raw_options& raw, std::ostream& error_stream)
        -> stdx::result<options, clap::error>;
};

} // namespace ghoti::cmd::format
