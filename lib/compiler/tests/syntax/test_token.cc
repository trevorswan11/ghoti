#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/syntax/token.hh"
#include "compiler/syntax/token_type.hh"

namespace ghoti::tests {

using syntax::token_type_t;

namespace {

auto test_token_promotion(std::string_view input, token_type_t type, std::string_view expected)
    -> void {
    const syntax::token_t tok{type, input, 0, 0};
    const auto            promoted{tok.materialize_string()};
    CHECK(promoted == expected);
}

auto test_string(std::string_view input, std::string_view expected) {
    test_token_promotion(input, token_type_t::STRING, expected);
}

auto test_ml_string(std::string_view input, std::string_view expected) {
    test_token_promotion(input, token_type_t::MULTILINE_STRING, expected);
}

} // namespace

TEST_CASE("Promotion of standard string literals") {
    test_string(R"("Hello, World!")", "Hello, World!");
    test_string(R"(""Hello, World!"")", R"("Hello, World!")");
    test_string(R"("")", "");
}

TEST_CASE("Promotion of multiline literals") {
    test_ml_string(R"(\\Hello,"World!")", R"(Hello,"World!")");
    test_ml_string("\\\\Hello,\n\\\\World!\n\\\\", "Hello,\nWorld!\n");
    test_ml_string(R"(\\)", "");
}

} // namespace ghoti::tests
