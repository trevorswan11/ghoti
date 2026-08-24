#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "driver/cmd/lsp/rpc.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("read_message parses a well-formed Content-Length frame") {
    const std::string  body{R"({"jsonrpc":"2.0","id":1,"method":"initialize"})"};
    std::istringstream in{"Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body};
    std::ostringstream errors;

    const auto message = UNWRAP(lsp::read_message(in, errors));
    CHECK(message.at("id") == 1);
    CHECK(message.at("method") == "initialize");
}

TEST_CASE("read_message header matching ignores case") {
    const std::string  body{R"({"jsonrpc":"2.0","method":"initialized"})"};
    std::istringstream in{"content-length: " + std::to_string(body.size()) + "\r\n\r\n" + body};
    std::ostringstream errors;

    const auto message = UNWRAP(lsp::read_message(in, errors));
    CHECK(message.at("method") == "initialized");
}

TEST_CASE("read_message returns none on a clean empty stream") {
    std::istringstream in;
    std::ostringstream errors;
    CHECK_FALSE(lsp::read_message(in, errors));
}

TEST_CASE("read_message returns none and logs when Content-Length is missing") {
    std::istringstream in{"Foo: bar\r\n\r\n{}"};
    std::ostringstream errors;
    CHECK_FALSE(lsp::read_message(in, errors));
    CHECK(errors.view().contains("Content-Length"));
}

TEST_CASE("read_message returns none and logs when the body is shorter than advertised") {
    std::istringstream in{"Content-Length: 100\r\n\r\n{}"};
    std::ostringstream errors;
    CHECK_FALSE(lsp::read_message(in, errors));
    CHECK(errors.view().contains("shorter"));
}

TEST_CASE("read_message returns none and logs on invalid JSON") {
    const std::string  body{"not json"};
    std::istringstream in{fmt::format("Content-Length: {}\r\n\r\n{}", body.size(), body)};
    std::ostringstream errors;
    CHECK_FALSE(lsp::read_message(in, errors));
    CHECK(errors.view().contains("parse"));
}

TEST_CASE("write_message frames the body with a matching byte-exact Content-Length") {
    std::ostringstream   out;
    const nlohmann::json message{{"jsonrpc", "2.0"}, {"method", "exit"}};
    lsp::write_message(out, message);

    const auto expected_body{message.dump()};
    CHECK(out.view() ==
          fmt::format("Content-Length: {}\r\n\r\n{}", expected_body.size(), expected_body));
}

TEST_CASE("write_message output round-trips through read_message") {
    std::stringstream    channel;
    const nlohmann::json sent{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "shutdown"}};
    lsp::write_message(channel, sent);

    std::stringstream errors;
    const auto        received = UNWRAP(lsp::read_message(channel, errors));
    CHECK(received == sent);
}

} // namespace ghoti::tests
