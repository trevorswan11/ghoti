#pragma once

#include <istream>
#include <ostream>
#include <string_view>

#include <nlohmann/json.hpp>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

namespace ghoti::lsp {

enum class read_failure : u8 {
    END_OF_STREAM,   // clean EOF between messages
    MALFORMED_FRAME, // unusable header or truncated body: the stream cannot be resynchronized
    INVALID_JSON,    // a well-framed body that is not JSON: the next message is still readable
};

// Reads one Content-Length framed JSON-RPC message
[[nodiscard]] auto read_framed_message(std::istream& in, std::ostream& error_stream)
    -> stdx::result<nlohmann::json, read_failure>;

// `read_framed_message`, collapsing every failure to none
[[nodiscard]] auto read_message(std::istream& in, std::ostream& error_stream)
    -> stdx::option<nlohmann::json>;

// Writes one JSON-RPC message with Content-Length framing and flushes immediately
auto write_message(std::ostream& out, const nlohmann::json& message) -> void;

[[nodiscard]] auto has_field(const nlohmann::json& message,
                             std::string_view      field,
                             std::string_view      needle) noexcept -> bool;

} // namespace ghoti::lsp
