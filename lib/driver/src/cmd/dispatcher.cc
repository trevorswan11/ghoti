#include "driver/cmd/dispatcher.hh"

#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "driver/cmd/debug.hh"

namespace ghoti::cmd {

auto dispatcher::operator()(debug& dump) -> stdx::result<void, i32> {
    PROFILE_FUNCTION();
    dump.run();
    return {};
}

} // namespace ghoti::cmd
