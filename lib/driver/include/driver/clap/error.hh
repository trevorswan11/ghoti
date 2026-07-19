#pragma once

#include <stdx/types.hh>

namespace ghoti::clap {

enum class error : u8 {
    INVALID_OPTIMIZATION = 10,
    MISSING_SUBCOMMAND,
};

} // namespace ghoti::clap
