#include <filesystem>
#include <fstream>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/base.h>
#include <fmt/ostream.h>
#include <gsl/util>

#include "compiler/codegen/llvm_scope.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "driver/clap/error.hh"
#include "driver/cmd/build_exe.hh"
#include "driver/cmd/build_lib.hh"
#include "support/tempfile.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("build_lib command execution") {
    SECTION("Non-existent input file returns FILE_NOT_FOUND") {
        codegen::llvm_scope scope;
        cmd::build_lib      cmd{"non_existent_file_12345.gh", "out.a", {}, {}};
        CHECK(UNWRAP_ERR(cmd.execute()) == clap::error::FILE_NOT_FOUND);
    }

    SECTION("Valid source file compiles and emits static library file") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_lib_source.gh"};
        tempfile            lib_file{"test_output.a"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                pub const add := fn(a: i64, b: i64): i64 {{
                    return a + b;
                }};
            )");
        }

        codegen::target_options    target_opts{.level = codegen::opt_level::O2};
        codegen::optimizer_options opt_opts{.level = codegen::opt_level::O2};

        cmd::build_lib cmd{src_file, lib_file, target_opts, opt_opts};
        REQUIRE(cmd.execute());
        CHECK(std::filesystem::exists(lib_file));
        CHECK(std::filesystem::file_size(lib_file) > 0);
    }

    SECTION("Cross-target compilation for Linux x86_64 emits static library") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_linux_lib_source.gh"};
        tempfile            lib_file{"test_linux_output.a"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                pub const mul := fn(x: i64, y: i64): i64 {{
                    return x * y;
                }};
            )");
        }

        codegen::target_options    target_opts{.triple_str = "x86_64-unknown-linux-gnu",
                                               .level      = codegen::opt_level::O2};
        codegen::optimizer_options opt_opts{.level = codegen::opt_level::O2};

        cmd::build_lib cmd{src_file, lib_file, target_opts, opt_opts};
        REQUIRE(cmd.execute());
        CHECK(std::filesystem::exists(lib_file));
        CHECK(std::filesystem::file_size(lib_file) > 0);
    }

    SECTION("Building static library and linking into executable") {
        codegen::llvm_scope scope;
        tempfile            lib_src{"math_lib.gh"};
        tempfile            lib_file{"libmath.a"};
        tempfile            exe_src{"app_main.gh"};
        tempfile            exe_file{"app_main"};

        {
            std::ofstream out{lib_src.path};
            fmt::print(out, R"(
                pub const compute := fn(x: i64): i64 {{
                    return x + 100l;
                }};
            )");
        }

        {
            std::ofstream out{exe_src.path};
            fmt::print(out, R"(
                pub const main := fn(args: [][:0]u8): void {{
                    return;
                }};
            )");
        }

        codegen::target_options    target_opts{.triple_str = "x86_64-unknown-linux-gnu",
                                               .level      = codegen::opt_level::O2};
        codegen::optimizer_options opt_opts{.level = codegen::opt_level::O2};

        // 1. Build static library
        cmd::build_lib lib_cmd{lib_src, lib_file, target_opts, opt_opts};
        REQUIRE(lib_cmd.execute());
        REQUIRE(std::filesystem::exists(lib_file));

        // 2. Build executable linking against the static library via extra_objects
        cmd::build_exe exe_cmd{exe_src, exe_file, target_opts, opt_opts, {}, {lib_file}};
        REQUIRE(exe_cmd.execute());
        CHECK(std::filesystem::exists(exe_file));
        CHECK(std::filesystem::file_size(exe_file) > 0);
    }
}

} // namespace ghoti::tests
