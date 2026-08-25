#include "support/tempfile.hh"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string_view>
#include <system_error>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <stdx/types.hh>

namespace ghoti {

namespace {

const u64        seed{std::random_device{}()};
std::atomic<u64> counter{0};

auto try_remove_path(const std::filesystem::path& path) -> void {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) { fmt::println(std::cerr, "Failed to remove tempfile: {}", ec.message()); }
}

} // namespace

tempfile::tempfile(std::string_view tag) : path{make_temp_path(tag)} { try_remove_path(path); }

tempfile::~tempfile() { try_remove_path(path); }

auto tempfile::make_temp_path(std::string_view tag) -> std::filesystem::path {
    const auto dir{std::filesystem::temp_directory_path()};
    const auto name{fmt::format("ghoti_{}_{}_{}", tag, seed, counter.fetch_add(1))};
    return dir / name;
}

tempdir::tempdir(std::string_view tag) : path{tempfile::make_temp_path(tag)} {
    std::filesystem::create_directories(path);
}

tempdir::~tempdir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

auto tempdir::write(const std::filesystem::path& relative, std::string_view content) const -> void {
    const auto full{path / relative};
    std::filesystem::create_directories(full.parent_path());
    std::ofstream out{full};
    out << content;
}

} // namespace ghoti
