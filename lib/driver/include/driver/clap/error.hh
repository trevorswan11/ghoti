#pragma once

#include <ostream>
#include <string>

#include <stdx/result.hh>
#include <stdx/types.hh>

namespace ghoti::clap {

enum class error : u8 {
    INVALID_OPTIMIZATION = 10,
    MISSING_SUBCOMMAND,
    MISSING_INPUT_FILE,
    FILE_NOT_FOUND,
    COMPILATION_FAILED,
    STDIN_LOAD_FAILED,
    INVALID_MODULE_SPEC,
};

[[nodiscard]] auto fatal_error(std::ostream& os, std::string message, error code)
    -> stdx::err<error>;

} // namespace ghoti::clap
