#include "support/path_utils.hh"

#include <cctype>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

#include <fmt/base.h>
#include <fmt/format.h>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>

#include "ghoti/config.h"
#include "support/string_utils.hh"

namespace ghoti::path_utils {

auto make_relative(const std::filesystem::path& path) -> stdx::option<std::filesystem::path> {
    if (path.is_absolute()) {
        std::error_code ec;
        auto            rel{std::filesystem::relative(path, ec)};
        if (!ec && !rel.empty()) { return rel; }
    }
    return stdx::none;
}

namespace {

auto percent_decode(std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size());
    for (usize i{0}; i < s.size(); ++i) {
        const auto has_hex_pair{i + 2 < s.size() && std::isxdigit(static_cast<u8>(s[i + 1])) != 0 &&
                                std::isxdigit(static_cast<u8>(s[i + 2])) != 0};
        if (s[i] == '%' && has_hex_pair) {
            const auto val{stdx::string::substr(s, i + 1, 2)};
            u8         byte;
            const auto res{std::from_chars(val.cbegin(), val.cend(), byte, 16)};
            ASSERT(res.ec == std::errc{} && res.ptr == val.cend());
            out.push_back(static_cast<char>(byte));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// Unreserved per RFC 3986, plus '/' as a path separator and ':' for the Windows drive letter;
// most LSP clients leave both unencoded in `file://` URIs, and matching that avoids URIs for the
// same file mismatching across a naive string comparison
auto is_unreserved(u8 c) -> bool {
    return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~' || c == '/' ||
           c == ':';
}

auto percent_encode(std::string_view s) -> std::string {
    std::string out;
    out.reserve(s.size());
    for (const auto raw_c : s) {
        const auto c{static_cast<u8>(raw_c)};
        if (is_unreserved(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            fmt::format_to(std::back_inserter(out), "%{:02X}", c);
        }
    }
    return out;
}

} // namespace

auto uri_to_path(std::string_view uri) -> stdx::option<std::filesystem::path> {
    PROFILE_FUNCTION();
    constexpr std::string_view scheme{"file://"};
    if (uri.size() < scheme.size() || uri.substr(0, scheme.size()) != scheme) { return stdx::none; }

    auto decoded{percent_decode(uri.substr(scheme.size()))};
#if GHOTI_WINDOWS
    // `file:///C:/...` carries a leading slash before the drive letter that Windows paths lack
    if (decoded.size() >= 3 && decoded[0] == '/' &&
        std::isalpha(static_cast<unsigned char>(decoded[1])) != 0 && decoded[2] == ':') {
        decoded.erase(0, 1);
    }
#endif
    return std::filesystem::path{string_utils::to_u8string(decoded)};
}

auto path_to_uri(const std::filesystem::path& path) -> std::string {
    PROFILE_FUNCTION();
    const auto utf8{string_utils::to_utf8(path.generic_u8string())};

#if GHOTI_WINDOWS
    const std::string_view prefix{utf8.size() >= 2 && utf8[1] == ':' ? "/" : ""};
#else
    const std::string_view prefix{};
#endif
    return fmt::format("file://{}{}", prefix, percent_encode(utf8));
}

} // namespace ghoti::path_utils
