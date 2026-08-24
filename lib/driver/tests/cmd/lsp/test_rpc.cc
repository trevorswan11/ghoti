#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
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
    CHECK_FALSE(lsp::read_message(in, errors).has_value());
}

TEST_CASE("read_message returns none and logs when Content-Length is missing") {
    std::istringstream in{"Foo: bar\r\n\r\n{}"};
    std::ostringstream errors;
    CHECK_FALSE(lsp::read_message(in, errors).has_value());
    CHECK(errors.str().find("Content-Length") != std::string::npos);
}

TEST_CASE("read_message returns none and logs when the body is shorter than advertised") {
    std::istringstream in{"Content-Length: 100\r\n\r\n{}"};
    std::ostringstream errors;
    CHECK_FALSE(lsp::read_message(in, errors).has_value());
    CHECK(errors.str().find("shorter") != std::string::npos);
}

TEST_CASE("read_message returns none and logs on invalid JSON") {
    const std::string  body{"not json"};
    std::istringstream in{"Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body};
    std::ostringstream errors;
    CHECK_FALSE(lsp::read_message(in, errors).has_value());
    CHECK(errors.str().find("parse") != std::string::npos);
}

TEST_CASE("write_message frames the body with a matching byte-exact Content-Length") {
    std::ostringstream   out;
    const nlohmann::json message{{"jsonrpc", "2.0"}, {"method", "exit"}};
    lsp::write_message(out, message);

    const auto expected_body{message.dump()};
    const auto expected{"Content-Length: " + std::to_string(expected_body.size()) + "\r\n\r\n" +
                        expected_body};
    CHECK(out.str() == expected);
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
