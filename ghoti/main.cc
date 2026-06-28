#include "driver/launch.hh"

auto main(int argc, char** argv) -> int {
    return ghoti::driver::launch(argc, argv).error_or(0);
}
