#include <iostream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>

#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "driver/clap/error.hh"
#include "driver/clap/parser.hh"
#include "driver/cmd/build_obj.hh"
#include "driver/cmd/repl.hh"
#include "helpers/argv.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace { codegen::llvm_global_target_init llvm_target_init_; } // namespace

TEST_CASE("Error with no args") {
    auto               args{helpers::mock_argv{"ghoti"}};
    std::ostringstream error_ss;
    clap::parser       parser{args.argc(), args.argv(), error_ss, false};
    CHECK(UNWRAP_ERR(parser.parse()) == clap::error::MISSING_SUBCOMMAND);
    CHECK_FALSE(error_ss.view().empty());
}

TEST_CASE("REPL subcommand parser") {
    auto         args{helpers::mock_argv{"ghoti", "repl"}};
    clap::parser parser{args.argc(), args.argv(), std::cerr, false};
    auto         cmd{UNWRAP(parser.parse())};
    CHECK(dynamic_cast<cmd::repl*>(cmd.get()));
}

TEST_CASE("build-obj subcommand parser") {
    SECTION("Basic positional input file with default output path") {
        auto         args{helpers::mock_argv{"ghoti", "build-obj", "src/main.gh"}};
        clap::parser parser{args.argc(), args.argv(), std::cerr, false};
        auto         cmd{UNWRAP(parser.parse())};
        auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};
        CHECK(build_cmd.get_input_path() == "src/main.gh");
        CHECK(build_cmd.get_output_path() == "src/main.o");
        CHECK(build_cmd.get_opt_opts().level == codegen::opt_level::O0);
    }

    SECTION("Explicit output path") {
        auto         args{helpers::mock_argv{"ghoti", "build-obj", "-o", "bin/out.o", "main.gh"}};
        clap::parser parser{args.argc(), args.argv(), std::cerr, false};
        auto         cmd{UNWRAP(parser.parse())};
        auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};
        CHECK(build_cmd.get_input_path() == "main.gh");
        CHECK(build_cmd.get_output_path() == "bin/out.o");
    }

    SECTION("Target options parsing") {
        auto         args{helpers::mock_argv{"ghoti",
                                     "build-obj",
                                     "--target",
                                     "x86_64-unknown-linux-gnu",
                                     "--cpu",
                                     "skylake",
                                     "--features",
                                     "+avx2",
                                     "main.gh"}};
        clap::parser parser{args.argc(), args.argv(), std::cerr, false};
        auto         cmd{UNWRAP(parser.parse())};
        auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};
        const auto&  target_opts{build_cmd.get_target_opts()};
        REQUIRE(target_opts.triple_str.has_value());
        CHECK(*target_opts.triple_str == "x86_64-unknown-linux-gnu");
        CHECK(target_opts.cpu == "skylake");
        CHECK(target_opts.features == "+avx2");
    }

    SECTION("Optimization flag parsing") {
        SECTION("Default is O0") {
            auto         args{helpers::mock_argv{"ghoti", "build-obj", "main.gh"}};
            clap::parser parser{args.argc(), args.argv(), std::cerr, false};
            auto         cmd{UNWRAP(parser.parse())};
            auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};
            CHECK(build_cmd.get_opt_opts().level == codegen::opt_level::O0);
        }

        SECTION("Release flag sets O2 default") {
            auto         args{helpers::mock_argv{"ghoti", "build-obj", "--release", "main.gh"}};
            clap::parser parser{args.argc(), args.argv(), std::cerr, false};
            auto         cmd{UNWRAP(parser.parse())};
            auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};
            CHECK(build_cmd.get_opt_opts().level == codegen::opt_level::O2);
        }

        SECTION("Explicit -O flags override default") {
            auto         args{helpers::mock_argv{"ghoti", "build-obj", "-O3", "main.gh"}};
            clap::parser parser{args.argc(), args.argv(), std::cerr, false};
            auto         cmd{UNWRAP(parser.parse())};
            auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};
            CHECK(build_cmd.get_opt_opts().level == codegen::opt_level::O3);
        }

        SECTION("Explicit -Os and -Oz flags") {
            auto         args{helpers::mock_argv{"ghoti", "build-obj", "-Os", "main.gh"}};
            clap::parser parser{args.argc(), args.argv(), std::cerr, false};
            auto         cmd{UNWRAP(parser.parse())};
            auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};
            CHECK(build_cmd.get_opt_opts().level == codegen::opt_level::Os);
        }

        SECTION("Pass debugging and timing flags") {
            auto         args{helpers::mock_argv{
                "ghoti", "build-obj", "--debug-passes", "--time-passes", "main.gh"}};
            clap::parser parser{args.argc(), args.argv(), std::cerr, false};
            auto         cmd{UNWRAP(parser.parse())};
            auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};
            CHECK(build_cmd.get_opt_opts().debug_logging);
            CHECK(build_cmd.get_opt_opts().time_passes);
        }

        SECTION("Invalid optimization level returns error") {
            auto args{helpers::mock_argv{"ghoti", "build-obj", "-Oinvalid", "main.gh"}};
            std::ostringstream error_ss;
            clap::parser       parser{args.argc(), args.argv(), error_ss, false};
            CHECK(UNWRAP_ERR(parser.parse()) == clap::error::INVALID_OPTIMIZATION);
            CHECK_FALSE(error_ss.view().empty());
        }
    }

    SECTION("Module argument parsing (-m / --module)") {
        SECTION("Single module argument") {
            auto args{
                helpers::mock_argv{"ghoti", "build-obj", "-m", "math,src/math.gh", "main.gh"}};
            clap::parser parser{args.argc(), args.argv(), std::cerr, false};
            auto         cmd{UNWRAP(parser.parse())};
            auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};
            const auto&  mods{build_cmd.get_modules()};
            REQUIRE(mods.size() == 1);
            CHECK(mods[0].name == "math");
            CHECK(mods[0].path == "src/math.gh");
        }

        SECTION("Multiple module arguments") {
            auto         args{helpers::mock_argv{"ghoti",
                                         "build-obj",
                                         "-m",
                                         "math,src/math.gh",
                                         "--module",
                                         "io,src/io.gh",
                                         "main.gh"}};
            clap::parser parser{args.argc(), args.argv(), std::cerr, false};
            auto         cmd{UNWRAP(parser.parse())};
            auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};
            const auto&  mods{build_cmd.get_modules()};
            REQUIRE(mods.size() == 2);
            CHECK(mods[0].name == "math");
            CHECK(mods[0].path == "src/math.gh");
            CHECK(mods[1].name == "io");
            CHECK(mods[1].path == "src/io.gh");
        }

        SECTION("Invalid module specification returns error") {
            auto args{
                helpers::mock_argv{"ghoti", "build-obj", "-m", "invalid_no_comma", "main.gh"}};
            std::ostringstream error_ss;
            clap::parser       parser{args.argc(), args.argv(), error_ss, false};
            CHECK(UNWRAP_ERR(parser.parse()) == clap::error::INVALID_MODULE_SPEC);
            CHECK_FALSE(error_ss.view().empty());
        }
    }
}

} // namespace ghoti::tests
