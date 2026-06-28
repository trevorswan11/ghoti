#include <iostream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>
#include <stdx/variant.hh>

#include "driver/clap/parser.hh"
#include "driver/cmd/debug.hh"
#include "helpers/argv.hh"

namespace ghoti::tests {

TEST_CASE("Error with no args") {
    auto               args{helpers::mock_argv{"ghoti"}};
    std::ostringstream error_ss;
    clap::parser       parser{args.argc(), args.argv(), error_ss, false};
    const auto         result{parser.parse()};
    REQUIRE_FALSE(result);
    CHECK(result.error() == 1);
    CHECK_FALSE(error_ss.view().empty());
}

TEST_CASE("Ast dump parser") {
    auto         args{helpers::mock_argv{"ghoti", "debug"}};
    clap::parser parser{args.argc(), args.argv(), std::cerr, false};
    CHECK(parser.get_parsed().is<stdx::monostate>());
    REQUIRE(parser.parse());
    CHECK(parser.get_parsed().is<cmd::debug>());
}

} // namespace ghoti::tests
