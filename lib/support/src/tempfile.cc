#include "support/tempfile.hh"

#include <atomic>
#include <filesystem>
#include <random>
#include <string_view>

#include <fmt/format.h>
#include <stdx/types.hh>

namespace ghoti {

namespace {

const u64        seed{std::random_device{}()};
std::atomic<u64> counter{0};

auto tempfile_path(std::string_view tag) -> std::filesystem::path {
    const auto dir{std::filesystem::temp_directory_path()};
    const auto name{fmt::format("ghoti_{}_{}_{}", tag, seed, counter.fetch_add(1))};
    return dir / name;
}

} // namespace

tempfile::tempfile(std::string_view tag) : path{tempfile_path(tag)} {
    std::filesystem::remove(path);
}

tempfile::~tempfile() { std::filesystem::remove(path); }

} // namespace ghoti
