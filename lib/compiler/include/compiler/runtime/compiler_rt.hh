#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/linker.hh"
#include "compiler/codegen/target.hh"

namespace ghoti::runtime {

// The builtins archive `object` needs, built from ghoti sources at most once per target per
// process. `none` when the object calls no runtime libcall or no compiler_rt sources exist.
[[nodiscard]] auto resolve_compiler_rt(const codegen::target_options&      target_opts,
                                       const std::filesystem::path&        object,
                                       const codegen::compiler_rt_options& options)
    -> stdx::result<stdx::option<std::filesystem::path>, codegen::diagnostic>;

// The root `compiler_rt.gh` that `options` selects, if it exists
[[nodiscard]] auto locate_compiler_rt(const codegen::compiler_rt_options& options)
    -> stdx::option<std::filesystem::path>;

// Undefined symbols in `object` that compiler_rt exists to provide, without the global prefix
[[nodiscard]] auto compiler_rt_imports(const std::filesystem::path&   object,
                                       const codegen::target_options& target_opts)
    -> stdx::result<std::vector<std::string>, codegen::diagnostic>;

// Functions whose own compiled body calls back into themselves through a runtime libcall, as in
// `__addtf3` written with `f128` addition. `asm_text` is the archive's assembly.
[[nodiscard]] auto find_self_calling_libcalls(std::string_view               asm_text,
                                              const codegen::target_options& target_opts)
    -> std::vector<std::string>;

// How many archives this process has built, for tests
[[nodiscard]] auto compiler_rt_builds() noexcept -> usize;

} // namespace ghoti::runtime
