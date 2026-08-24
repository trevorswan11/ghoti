#include "driver/cmd/lsp/rpc.hh"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <ios>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <nlohmann/json.hpp>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>
#include <system_error>

namespace ghoti::lsp {

namespace {

// getline splits on '\n', leaving a trailing '\r' behind on CRLF-framed headers
auto strip_trailing_cr(std::string& line) -> void {
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
}

// Parses a "Content-Length: <n>" header; header name matching is case-insensitive per the spec
auto try_parse_content_length(std::string_view line) -> stdx::option<usize> {
    constexpr std::string_view prefix{"Content-Length:"};
    if (!std::ranges::starts_with(
            line, prefix, {}, stdx::string::to_lower, stdx::string::to_lower)) {
        return stdx::none;
    }

    auto value{line.substr(prefix.size())};
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
        strip_trailing_cr(line);
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

    // Brace-init here would hit nlohmann's single-element-wraps-in-an-array pitfall
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

} // namespace ghoti::lsp
