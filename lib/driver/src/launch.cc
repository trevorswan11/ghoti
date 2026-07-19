#include "driver/launch.hh"

#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "driver/clap/parser.hh"

namespace ghoti::driver {

auto launch(i32 argc, char** argv) -> i32 {
    stdx::profiler profiler{argv[0]};
    clap::parser   parser{argc, argv};

    const auto command{parser.parse().transform_error([](auto e) { return static_cast<i32>(e); })};
    if (command) {
        const auto exec_res{(*command)->execute()};
        return exec_res ? 0 : static_cast<i32>(exec_res.error());
    }
    return command.error();
}

} // namespace ghoti::driver
