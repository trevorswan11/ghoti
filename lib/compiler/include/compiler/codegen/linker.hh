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
    // The root `compiler_rt.gh`; `GHOTI_COMPILER_RT` default, then `lib/compiler_rt`
    stdx::option<std::filesystem::path> root{};
};

// A symbol a program imports from an `extern("lib", ...)` library
struct library_import {
    std::string library;
    std::string symbol;
    bool        data{false};
};

struct extra_linker_options {
    gsl::span<std::filesystem::path> objects{};
    gsl::span<std::filesystem::path> library_paths{};
    gsl::span<std::string>           libraries{};
    // Compiler builtins archive, linked after every other input so it only fills in what's missing
    stdx::option<std::filesystem::path> builtins{};
    // Builds `builtins` from ghoti sources when the object calls a runtime libcall; unset skips it
    stdx::option<compiler_rt_options> compiler_rt{};
    // What each library supplies; a Windows import library that can't be found is written from it
    gsl::span<const library_import> imports{};
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
