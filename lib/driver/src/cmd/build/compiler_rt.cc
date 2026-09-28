#include "driver/cmd/build/compiler_rt.hh"

#include <filesystem>

#include <stdx/option.hh>

#include "compiler/codegen/target.hh"

namespace ghoti::cmd::build {

auto resolve_compiler_rt([[maybe_unused]] const codegen::target_options& target_opts)
    -> stdx::option<std::filesystem::path> {
    // TODO(tcs): compile `lib/compiler_rt/compiler_rt.gh` for the target and cache it
    return stdx::none;
}

} // namespace ghoti::cmd::build
