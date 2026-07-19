#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "driver/launch.hh"

auto main(i32 argc, char** argv) -> i32 {
    stdx::profiler profiler{argv[0]};
    return ghoti::driver::launch(argc, argv);
}
