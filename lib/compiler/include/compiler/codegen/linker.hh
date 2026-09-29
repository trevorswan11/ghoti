#pragma once

#include <filesystem>
#include <string>

#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/target.hh"

namespace ghoti::codegen {

// How the compiler builtins archive is located and built when `builtins` isn't given
struct compiler_rt_options {
    // The root `compiler_rt.gh`; defaults to `GHOTI_COMPILER_RT`, then the shipped
    // `lib/compiler_rt`
    stdx::option<std::filesystem::path> root{};
};

struct extra_linker_options {
    gsl::span<std::filesystem::path> objects{};
    gsl::span<std::filesystem::path> library_paths{};
    gsl::span<std::string>           libraries{};
    // Set when the entry wrapper calls the raw Win32 APIs used to recover real argv.
    bool needs_windows_argv_apis{false};
    // Compiler builtins archive, linked after every other input so it only fills in what's missing
    stdx::option<std::filesystem::path> builtins{};
    // Builds `builtins` from ghoti sources when the object calls a runtime libcall; unset skips it
    stdx::option<compiler_rt_options> compiler_rt{};
};

// True when an import-lib directory for the Windows Win32 APIs (kernel32 / shell32) is configured
[[nodiscard]] auto has_windows_argv_sysroot() -> bool;

[[nodiscard]] auto link_executable(const std::filesystem::path& object_file,
                                   const std::filesystem::path& output_file,
                                   const target_options&        target_opts,
                                   const extra_linker_options&  linker_opts = {})
    -> stdx::result<void, diagnostic>;

[[nodiscard]] auto create_static_library(const std::filesystem::path&           output_file,
                                         gsl::span<const std::filesystem::path> object_files,
                                         const target_options&                  target_opts)
    -> stdx::result<void, diagnostic>;

[[nodiscard]] auto link_dynamic_library(const std::filesystem::path& object_file,
                                        const std::filesystem::path& output_file,
                                        const target_options&        target_opts,
                                        const extra_linker_options&  linker_opts = {})
    -> stdx::result<void, diagnostic>;

} // namespace ghoti::codegen
