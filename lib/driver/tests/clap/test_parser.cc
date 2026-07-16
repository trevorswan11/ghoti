#include <iostream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>
#include <stdx/variant.hh>

#include "compiler/codegen/opt_level.hh"
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
    CHECK(parser.get_opt_options().level == codegen::opt_level::O0);
}

TEST_CASE("Optimization flag parser") {
    SECTION("Default is O0") {
        auto         args{helpers::mock_argv{"ghoti", "debug"}};
        clap::parser parser{args.argc(), args.argv(), std::cerr, false};
        REQUIRE(parser.parse());
        CHECK(parser.get_opt_options().level == codegen::opt_level::O0);
    }

    SECTION("Release flag sets O2 default") {
        auto         args{helpers::mock_argv{"ghoti", "--release", "debug"}};
        clap::parser parser{args.argc(), args.argv(), std::cerr, false};
        REQUIRE(parser.parse());
        CHECK(parser.get_opt_options().level == codegen::opt_level::O2);
    }

    SECTION("Explicit -O flags override default") {
        auto         args{helpers::mock_argv{"ghoti", "-O3", "debug"}};
        clap::parser parser{args.argc(), args.argv(), std::cerr, false};
        REQUIRE(parser.parse());
        CHECK(parser.get_opt_options().level == codegen::opt_level::O3);
    }

    SECTION("Explicit -Os and -Oz flags") {
        auto         args{helpers::mock_argv{"ghoti", "-Os", "debug"}};
        clap::parser parser{args.argc(), args.argv(), std::cerr, false};
        REQUIRE(parser.parse());
        CHECK(parser.get_opt_options().level == codegen::opt_level::Os);
    }

    SECTION("Pass debugging and timing flags") {
        auto         args{helpers::mock_argv{"ghoti", "--debug-passes", "--time-passes", "debug"}};
        clap::parser parser{args.argc(), args.argv(), std::cerr, false};
        REQUIRE(parser.parse());
        CHECK(parser.get_opt_options().debug_logging);
        CHECK(parser.get_opt_options().time_passes);
    }

    SECTION("Invalid optimization level returns error") {
        auto               args{helpers::mock_argv{"ghoti", "-Oinvalid", "debug"}};
        std::ostringstream error_ss;
        clap::parser       parser{args.argc(), args.argv(), error_ss, false};
        const auto         result{parser.parse()};
        REQUIRE_FALSE(result);
        CHECK(result.error() == 1);
        CHECK_FALSE(error_ss.view().empty());
    }
}

} // namespace ghoti::tests
