#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "driver/cmd/lsp/formatting.hh"

namespace ghoti::tests {

TEST_CASE("lsp::format generates LSP TextEdit array or nullptr") {
    SECTION("unformatted code returns a single TextEdit covering entire document") {
        constexpr std::string_view unformatted{"const y:i32=42;\n"};
        const auto                 edits = lsp::format(unformatted);
        REQUIRE(edits.is_array());
        REQUIRE(edits.size() == 1);
        CHECK(edits[0].at("newText") == "const y: i32 = 42;\n");
        CHECK(edits[0].at("range").at("start").at("line") == 0);
        CHECK(edits[0].at("range").at("start").at("character") == 0);
        CHECK(edits[0].at("range").at("end").at("line") == 1);
        CHECK(edits[0].at("range").at("end").at("character") == 0);
    }

    SECTION("already formatted code returns empty edits array") {
        constexpr std::string_view formatted{"const y: i32 = 42;\n"};
        const auto                 edits = lsp::format(formatted);
        REQUIRE(edits.is_array());
        CHECK(edits.empty());
    }

    SECTION("code with syntax error returns nullptr") {
        constexpr std::string_view invalid{"const broken := ;\n"};
        const auto                 edits = lsp::format(invalid);
        CHECK(edits.is_null());
    }
}

} // namespace ghoti::tests
