#include <filesystem>
#include <fstream>
#include <system_error>

#include <catch2/catch_test_macros.hpp>
#include <fmt/ostream.h>
#include <stdx/types.hh>

#include "compiler/codegen/llvm_scope.hh"
#include "driver/clap/error.hh"
#include "driver/cmd/build/run.hh"
#include "support/tempfile.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("run command execution") {
    SECTION("Non-existent input file returns FILE_NOT_FOUND") {
        codegen::llvm_scope scope;
        cmd::run_cmd        cmd{{.input_path = "non_existent_run_12345.gh"}};
        CHECK(UNWRAP_ERR(cmd.execute()) == clap::error::FILE_NOT_FOUND);
    }

    SECTION("A program returning 0 compiles, runs, and succeeds") {
        codegen::llvm_scope scope;
        tempfile            src_file{"run_driver_ok.gh"};
        {
            std::ofstream out{src_file.path};
            fmt::print(out, "pub const main := fn(): void {{ return; }};");
        }

        cmd::run_cmd cmd{{.input_path = src_file}};
        REQUIRE(cmd.execute());
    }

    SECTION("The program's exit code is propagated") {
        codegen::llvm_scope scope;
        tempfile            src_file{"run_driver_exit_code.gh"};
        {
            std::ofstream out{src_file.path};
            fmt::print(out, "pub const main := fn(): i32 {{ return 7; }};");
        }

        cmd::run_cmd cmd{{.input_path = src_file}};
        CHECK(UNWRAP_ERR(cmd.execute()) == static_cast<clap::error>(7));
    }

    SECTION("Forwarded program args reach the binary's argv") {
        codegen::llvm_scope scope;
        tempfile            src_file{"run_driver_args.gh"};
        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                pub const main := fn(args: [][:0]u8): i32 {{
                    if (args.len == 3) {{ return 0; }} // exe + two forwarded
                    if (args.len == 0) {{ return 0; }} // host without an argv sysroot
                    return 4;
                }};
            )");
        }

        cmd::run_cmd cmd{{
            .input_path     = src_file,
            .forwarded_args = {"alpha", "beta"},
        }};
        // An `args`-taking `main` needs a Windows argv sysroot to link; tolerate its absence.
        if (const auto res{cmd.execute()}; !res) {
            CHECK(res.error() == clap::error::COMPILATION_FAILED);
        }
    }

    SECTION("The temporary binary is removed after the run") {
        codegen::llvm_scope scope;
        tempfile            src_file{"run_driver_cleanup.gh"};
        {
            std::ofstream out{src_file.path};
            fmt::print(out, "pub const main := fn(): void {{ return; }};");
        }

        cmd::run_cmd cmd{{.input_path = src_file}};
        REQUIRE(cmd.execute());

        std::error_code ec;
        CHECK(!cmd.get_opts().output_path.empty());
        CHECK(!std::filesystem::exists(cmd.get_opts().output_path, ec));
    }
}

} // namespace ghoti::tests
