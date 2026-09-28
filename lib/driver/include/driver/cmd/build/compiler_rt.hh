#pragma once

#include <filesystem>

#include <stdx/option.hh>

#include "compiler/codegen/target.hh"

namespace ghoti::cmd::build {

// The archive of compiler builtins that LLVM may call into for the target
[[nodiscard]] auto resolve_compiler_rt(const codegen::target_options& target_opts)
    -> stdx::option<std::filesystem::path>;

} // namespace ghoti::cmd::build
