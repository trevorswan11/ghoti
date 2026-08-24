#include "driver/cmd/format/formatter.hh"

#include <stdx/result.hh>
#include <stdx/utility.hh>

#include "driver/clap/error.hh"

namespace ghoti::cmd {

auto formatter::execute() -> stdx::result<void, clap::error> {
    TODO("Connect to syntax formatter");
}

} // namespace ghoti::cmd
