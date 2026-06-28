#pragma once

#include <stdx/result.hh>
#include <stdx/types.hh>

namespace ghoti::driver {

// Parses command line arguments and dispatches the input
auto launch(i32 argc, char** argv) -> stdx::result<void, i32>;

} // namespace ghoti::driver
