#include "driver/launch.hh"

#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "driver/clap/parser.hh"
#include "driver/cmd/dispatcher.hh"

namespace ghoti::driver {

auto launch(i32 argc, char** argv) -> stdx::result<void, i32> {
    stdx::profiler profiler{argv[0]};
    clap::parser   parser{argc, argv};
    TRY(parser.parse());

    cmd::dispatcher dispatcher;
    return parser.get_parsed().visit(dispatcher).error_or(0);
}

} // namespace ghoti::driver
