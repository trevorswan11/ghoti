#pragma once

#include <filesystem>
#include <string>

#include <gsl/span>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/target.hh"

namespace ghoti::codegen {

struct extra_linker_options {
    gsl::span<std::filesystem::path> objects{};
    gsl::span<std::filesystem::path> library_paths{};
    gsl::span<std::string>           libraries{};
    // Set when the entry wrapper calls the raw Win32 APIs used to recover real argv.
    bool needs_windows_argv_apis{false};
};

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
