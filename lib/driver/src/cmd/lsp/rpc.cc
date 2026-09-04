#include "driver/cmd/lsp/rpc.hh"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <ios>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <nlohmann/json.hpp>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>

#include "support/string_utils.hh"

namespace ghoti::lsp {

namespace {

// Parses a "Content-Length: <n>" header; header name matching is case-insensitive per the spec
auto try_parse_content_length(std::string_view line) -> stdx::option<usize> {
    constexpr std::string_view prefix{"Content-Length:"};
    if (!string_utils::starts_with_ci(line, prefix)) { return stdx::none; }

    auto value{stdx::string::substr(line, prefix.size())};
    while (!value.empty() && value.front() == ' ') { value.remove_prefix(1); }

    usize      length{0};
    const auto res{std::from_chars(value.cbegin(), value.cend(), length)};
    if (res.ec != std::errc{} || res.ptr != value.cend()) { return stdx::none; }
    return length;
}

} // namespace

auto read_message(std::istream& in, std::ostream& error_stream) -> stdx::option<nlohmann::json> {
    PROFILE_FUNCTION();
    stdx::option<usize> content_length;
    std::string         line;

    while (std::getline(in, line)) {
        string_utils::strip_trailing_cr(line);
        if (line.empty()) { break; }
        if (auto length{try_parse_content_length(line)}) { content_length = length; }
    }

    // A clean pipe close between messages surfaces as EOF with nothing parsed yet
    if (!content_length) {
        if (!in.eof()) { fmt::println(error_stream, "lsp: message missing Content-Length header"); }
        return stdx::none;
    }

    std::string body(*content_length, '\0');
    in.read(body.data(), static_cast<std::streamsize>(body.size()));
    if (static_cast<usize>(in.gcount()) != *content_length) {
        fmt::println(error_stream, "lsp: message body shorter than Content-Length");
        return stdx::none;
    }

    auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded()) {
        fmt::println(error_stream, "lsp: failed to parse message body as JSON");
        return stdx::none;
    }
    return parsed;
}

auto write_message(std::ostream& out, const nlohmann::json& message) -> void {
    PROFILE_FUNCTION();
    const auto body{message.dump()};
    fmt::print(out, "Content-Length: {}\r\n\r\n{}", body.size(), body); // registered nurse
    out.flush();
}

auto has_field(const nlohmann::json& message,
               std::string_view      field,
               std::string_view      needle) noexcept -> bool {
    try {
        return std::ranges::any_of(message, [&](const auto& s) {
            return s.at(field).template get<std::string>() == needle;
        });
    } catch (...) { return false; }
}

} // namespace ghoti::lsp
