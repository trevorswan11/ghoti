#include <iostream>
#include <sstream>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "driver/clap/error.hh"
#include "driver/clap/parser.hh"
#include "driver/cmd/build/executable.hh"
#include "driver/cmd/build/library.hh"
#include "driver/cmd/build/object.hh"
#include "driver/cmd/repl/shell.hh"
#include "support/subprocess.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("Error with no args") {
    mock_argv          args{"ghoti"};
    std::ostringstream error_ss;
    clap::parser       parser{args.argc(), args.argv(), error_ss, false};
    CHECK(UNWRAP_ERR(parser.parse()) == clap::error::MISSING_SUBCOMMAND);
    CHECK_FALSE(error_ss.view().empty());
}

TEST_CASE("REPL subcommand parser") {
    mock_argv    args{"ghoti", "repl"};
    clap::parser parser{args.argc(), args.argv(), std::cerr, false};
    auto         cmd{UNWRAP(parser.parse())};
    CHECK(dynamic_cast<cmd::shell*>(cmd.get()));
}

TEST_CASE("build-obj subcommand parser") {
    SECTION("Basic positional input file with default output path") {
        mock_argv    args{"ghoti", "build-obj", "src/main.gh"};
        clap::parser parser{args.argc(), args.argv(), std::cerr, false};
        auto         cmd{UNWRAP(parser.parse())};
        auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};

        const auto& opts{build_cmd.get_opts()};
        CHECK(opts.input_path == "src/main.gh");
        const auto expected_default{fmt::format(
            "src/main{}", codegen::get_default_output_extension(codegen::output_type::OBJECT))};
        CHECK(opts.output_path == expected_default);
        CHECK(opts.opt_opts.level == codegen::opt_level::O0);
    }

    SECTION("Explicit output path") {
        mock_argv    args{"ghoti", "build-obj", "-o", "bin/out.o", "main.gh"};
        clap::parser parser{args.argc(), args.argv(), std::cerr, false};
        auto         cmd{UNWRAP(parser.parse())};
        auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};

        const auto& opts{build_cmd.get_opts()};
        CHECK(opts.input_path == "main.gh");
        CHECK(opts.output_path == "bin/out.o");
    }

    SECTION("Target options parsing") {
        mock_argv    args{"ghoti",
                       "build-obj",
                       "--target",
                       "x86_64-unknown-linux-gnu",
                       "--cpu",
                       "skylake",
                       "--features",
                       "+avx2",
                       "main.gh"};
        clap::parser parser{args.argc(), args.argv(), std::cerr, false};
        auto         cmd{UNWRAP(parser.parse())};
        auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};
        const auto&  target_opts{build_cmd.get_opts().target_opts};
        CHECK(UNWRAP(target_opts.triple_str) == "x86_64-unknown-linux-gnu");
        CHECK(target_opts.cpu == "skylake");
        CHECK(target_opts.features == "+avx2");
    }

    SECTION("Optimization flag parsing") {
        SECTION("Default is O0") {
            mock_argv    args{"ghoti", "build-obj", "main.gh"};
            clap::parser parser{args.argc(), args.argv(), std::cerr, false};
            auto         cmd{UNWRAP(parser.parse())};
            auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};
            const auto&  opts{build_cmd.get_opts()};
            CHECK(opts.opt_opts.level == codegen::opt_level::O0);
        }

        SECTION("Release flag sets O2 default") {
            mock_argv    args{"ghoti", "build-obj", "--release", "main.gh"};
            clap::parser parser{args.argc(), args.argv(), std::cerr, false};
            auto         cmd{UNWRAP(parser.parse())};
            auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};
            const auto&  opts{build_cmd.get_opts()};
            CHECK(opts.opt_opts.level == codegen::opt_level::O2);
        }

        SECTION("Explicit -O flags override default") {
            mock_argv    args{"ghoti", "build-obj", "-O3", "main.gh"};
            clap::parser parser{args.argc(), args.argv(), std::cerr, false};
            auto         cmd{UNWRAP(parser.parse())};
            auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};
            const auto&  opts{build_cmd.get_opts()};
            CHECK(opts.opt_opts.level == codegen::opt_level::O3);
        }

        SECTION("Explicit -Os and -Oz flags") {
            mock_argv    args{"ghoti", "build-obj", "-Os", "main.gh"};
            clap::parser parser{args.argc(), args.argv(), std::cerr, false};
            auto         cmd{UNWRAP(parser.parse())};
            auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};
            const auto&  opts{build_cmd.get_opts()};
            CHECK(opts.opt_opts.level == codegen::opt_level::Os);
        }

        SECTION("Pass debugging and timing flags") {
            mock_argv    args{"ghoti", "build-obj", "--debug-passes", "--time-passes", "main.gh"};
            clap::parser parser{args.argc(), args.argv(), std::cerr, false};
            auto         cmd{UNWRAP(parser.parse())};
            auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};
            const auto&  opts{build_cmd.get_opts()};
            CHECK(opts.opt_opts.debug_logging);
            CHECK(opts.opt_opts.time_passes);
        }

        SECTION("Invalid optimization level returns error") {
            mock_argv          args{"ghoti", "build-obj", "-Oinvalid", "main.gh"};
            std::ostringstream error_ss;
            clap::parser       parser{args.argc(), args.argv(), error_ss, false};
            CHECK(UNWRAP_ERR(parser.parse()) == clap::error::INVALID_OPTIMIZATION);
            CHECK_FALSE(error_ss.view().empty());
        }
    }

    SECTION("Module argument parsing (-m / --module)") {
        SECTION("Single module argument") {
            mock_argv    args{"ghoti", "build-obj", "-m", "math,src/math.gh", "main.gh"};
            clap::parser parser{args.argc(), args.argv(), std::cerr, false};
            auto         cmd{UNWRAP(parser.parse())};
            auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};
            const auto&  mods{build_cmd.get_opts().modules};
            REQUIRE(mods.size() == 1);
            CHECK(mods[0].name == "math");
            CHECK(mods[0].path == "src/math.gh");
        }

        SECTION("Multiple module arguments") {
            mock_argv    args{"ghoti",
                           "build-obj",
                           "-m",
                           "math,src/math.gh",
                           "--module",
                           "io,src/io.gh",
                           "main.gh"};
            clap::parser parser{args.argc(), args.argv(), std::cerr, false};
            auto         cmd{UNWRAP(parser.parse())};
            auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_obj*>(cmd.get()))};
            const auto&  mods{build_cmd.get_opts().modules};
            REQUIRE(mods.size() == 2);
            CHECK(mods[0].name == "math");
            CHECK(mods[0].path == "src/math.gh");
            CHECK(mods[1].name == "io");
            CHECK(mods[1].path == "src/io.gh");
        }

        SECTION("Invalid module specification returns error") {
            mock_argv          args{"ghoti", "build-obj", "-m", "invalid_no_comma", "main.gh"};
            std::ostringstream error_ss;
            clap::parser       parser{args.argc(), args.argv(), error_ss, false};
            CHECK(UNWRAP_ERR(parser.parse()) == clap::error::INVALID_MODULE_SPEC);
            CHECK_FALSE(error_ss.view().empty());
        }
    }
}

TEST_CASE("build-exe subcommand parser") {
    SECTION("Basic positional input file with options") {
        mock_argv    args{"ghoti", "build-exe", "-o", "bin/myprog", "src/main.gh"};
        clap::parser parser{args.argc(), args.argv(), std::cerr, false};
        auto         cmd{UNWRAP(parser.parse())};
        auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_exe*>(cmd.get()))};

        const auto& opts{build_cmd.get_opts()};
        CHECK(opts.input_path == "src/main.gh");
        CHECK(opts.output_path == "bin/myprog");
        CHECK(opts.opt_opts.level == codegen::opt_level::O0);
    }
}

TEST_CASE("build-lib subcommand parser") {
    SECTION("Basic positional input file with default output path") {
        mock_argv    args{"ghoti", "build-lib", "src/lib.gh"};
        clap::parser parser{args.argc(), args.argv(), std::cerr, false};
        auto         cmd{UNWRAP(parser.parse())};
        auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_lib*>(cmd.get()))};

        const auto& opts{build_cmd.get_opts()};
        CHECK(opts.input_path == "src/lib.gh");
        const auto expected_default{fmt::format(
            "src/lib{}",
            codegen::get_default_output_extension(codegen::output_type::STATIC_LIBRARY))};
        CHECK(opts.output_path == expected_default);
        CHECK(opts.opt_opts.level == codegen::opt_level::O0);
        CHECK_FALSE(opts.dynamic);
    }

    SECTION("Explicit output path") {
        mock_argv    args{"ghoti", "build-lib", "-o", "lib/mylib.a", "lib.gh"};
        clap::parser parser{args.argc(), args.argv(), std::cerr, false};
        auto         cmd{UNWRAP(parser.parse())};
        auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_lib*>(cmd.get()))};

        const auto& opts{build_cmd.get_opts()};
        CHECK(opts.input_path == "lib.gh");
        CHECK(opts.output_path == "lib/mylib.a");
    }

    SECTION("Dynamic library flag parsing (--dynamic / --shared)") {
        mock_argv    args{"ghoti", "build-lib", "--dynamic", "src/lib.gh"};
        clap::parser parser{args.argc(), args.argv(), std::cerr, false};
        auto         cmd{UNWRAP(parser.parse())};
        auto&        build_cmd{UNWRAP(dynamic_cast<cmd::build_lib*>(cmd.get()))};

        const auto& opts{build_cmd.get_opts()};
        CHECK(opts.input_path == "src/lib.gh");
        CHECK(opts.dynamic);
    }
}

} // namespace ghoti::tests
