#pragma once

#include <filesystem>
#include <string>

#include <stdx/result.hh>
#include <stdx/types.hh>
#include <gsl/span>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/target.hh"

namespace ghoti::codegen {

struct extra_linker_options {
    gsl::span<std::filesystem::path> objects{};
    gsl::span<std::filesystem::path> library_paths{};
    gsl::span<std::string>           libraries{};
};

[[nodiscard]] auto link_executable(const std::filesystem::path& object_file,
                                   const std::filesystem::path& output_file,
                                   const target_options&        target_opts,
                                   const extra_linker_options&  linker_opts = {})
    -> stdx::result<void, diagnostic>;

} // namespace ghoti::codegen
