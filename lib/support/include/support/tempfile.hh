#pragma once

#include <filesystem>
#include <string_view>

#include <stdx/utility.hh>
#include <utility>

namespace ghoti {

struct tempfile {
    std::filesystem::path path;

    // Generates a path for a tempfile and ensures it does not exist on disk
    explicit tempfile(std::string_view tag);

    // Uses the path verbatim and only closes it on destruction
    explicit tempfile(std::in_place_t, std::filesystem::path p) noexcept : path{std::move(p)} {}
    ~tempfile();
    MAKE_PINNED(tempfile);

    [[nodiscard]] operator std::filesystem::path() const noexcept { return path; }
};

} // namespace ghoti
