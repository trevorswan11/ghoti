#pragma once

#include <istream>
#include <ostream>

#include <nlohmann/json.hpp>
#include <stdx/option.hh>

namespace ghoti::lsp {

// Reads one Content-Length framed JSON-RPC message; none on clean EOF or a malformed frame
[[nodiscard]] auto read_message(std::istream& in, std::ostream& error_stream)
    -> stdx::option<nlohmann::json>;

// Writes one JSON-RPC message with Content-Length framing and flushes immediately
auto write_message(std::ostream& out, const nlohmann::json& message) -> void;

} // namespace ghoti::lsp
