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
    CONFLICTING_OPTIONS,
    UNEXPECTED_ERROR,
    FORMATTING_FAILED,
    IO_ERROR,
};

[[nodiscard]] auto fatal_error(std::ostream& os, std::string message, error code)
    -> stdx::err<error>;

auto warn_error(std::ostream& os, std::string message) -> void;

} // namespace ghoti::clap
