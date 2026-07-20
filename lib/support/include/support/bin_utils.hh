#pragma once

#include <filesystem>

#include <gsl/span>

namespace ghoti::bin_utils {

// Checks length and contents for the ELF magic header
[[nodiscard]] auto check_elf_header(gsl::span<char> contents) -> bool;

// Check ELF magic header in a file
[[nodiscard]] auto check_elf_header(const std::filesystem::path& path) -> bool;

} // namespace ghoti::bin_utils
