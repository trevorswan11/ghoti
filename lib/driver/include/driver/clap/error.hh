#pragma once

#include <stdx/types.hh>

namespace ghoti::clap {

enum class error : u8 {
    INVALID_OPTIMIZATION = 10,
    MISSING_SUBCOMMAND,
    MISSING_INPUT_FILE,
    FILE_NOT_FOUND,
    COMPILATION_FAILED,
};

} // namespace ghoti::clap
