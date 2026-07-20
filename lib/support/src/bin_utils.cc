#include "support/bin_utils.hh"

#include <array>
#include <filesystem>
#include <fstream>
#include <ios>

#include <gsl/span>

namespace ghoti::bin_utils {

auto check_elf_header(gsl::span<char> contents) -> bool {
    if (contents.size() < 4) { return false; }
    if (contents[0] != 0x7f) { return false; }
    if (contents[1] != 'E') { return false; }
    if (contents[2] != 'L') { return false; }
    if (contents[3] != 'F') { return false; }
    return true;
}

auto check_elf_header(const std::filesystem::path& path) -> bool {
    std::ifstream       file{path, std::ios::binary};
    std::array<char, 4> header{};
    file.read(header.data(), 4);
    return check_elf_header(header);
}

} // namespace ghoti::bin_utils
