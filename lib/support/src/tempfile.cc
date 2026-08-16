#include "support/tempfile.hh"

#include <atomic>
#include <exception>
#include <filesystem>
#include <fmt/ostream.h>
#include <iostream>
#include <random>
#include <string_view>

#include <fmt/format.h>
#include <stdx/types.hh>

namespace ghoti {

namespace {

const u64        seed{std::random_device{}()};
std::atomic<u64> counter{0};

auto try_remove_path(const std::filesystem::path& path) -> void {
    try {
        std::filesystem::remove(path);
    } catch (std::exception& e) {
        fmt::println(std::cerr, "Failed to remove tempfile: {}", e.what());
    }
}

} // namespace

tempfile::tempfile(std::string_view tag) : path{make_temp_path(tag)} { try_remove_path(path); }

tempfile::~tempfile() { try_remove_path(path); }

auto tempfile::make_temp_path(std::string_view tag) -> std::filesystem::path {
    const auto dir{std::filesystem::temp_directory_path()};
    const auto name{fmt::format("ghoti_{}_{}_{}", tag, seed, counter.fetch_add(1))};
    return dir / name;
}

} // namespace ghoti
