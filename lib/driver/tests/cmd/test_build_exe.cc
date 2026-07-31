#include <filesystem>
#include <fstream>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/ostream.h>

#include "compiler/codegen/llvm_scope.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "driver/clap/error.hh"
#include "driver/cmd/build_exe.hh"
#include "driver/cmd/build_options.hh"
#include "support/bin_utils.hh"
#include "support/tempfile.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("build_exe command execution") {
    SECTION("Non-existent input file returns FILE_NOT_FOUND") {
        codegen::llvm_scope scope;
        cmd::build_exe      cmd{{
                 .input_path  = "non_existent_file_12345.gh",
                 .output_path = "out_exe",
        }};
        CHECK(UNWRAP_ERR(cmd.execute()) == clap::error::FILE_NOT_FOUND);
    }

    SECTION("Valid source file with pub main compiles and emits executable file") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_exe_source.gh"};
        tempfile            exe_file{"test_exe_output"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                pub const main := fn(args: [][:0]u8): void {{
                    return;
                }};
            )");
        }

        codegen::target_options    target_opts{.triple_str = "x86_64-unknown-linux-gnu",
                                               .level      = codegen::opt_level::O2};
        codegen::optimizer_options opt_opts{.level = codegen::opt_level::O2};

        cmd::build_exe cmd{{
            .input_path  = src_file,
            .output_path = exe_file,
            .target_opts = target_opts,
            .opt_opts    = opt_opts,
        }};
        REQUIRE(cmd.execute());
        CHECK(std::filesystem::exists(exe_file));
        CHECK(std::filesystem::file_size(exe_file) > 0);
    }

    SECTION("Valid main returning i32 directly with @as(i32, args.len) compiles and emits "
            "executable file") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_exe_as_cast.gh"};
        tempfile            exe_file{"test_exe_as_cast_out"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                pub const main := fn(args: [][:0]u8): i32 {{
                    return @as(i32, args.len);
                }};
            )");
        }

        codegen::target_options    target_opts{.triple_str = "x86_64-unknown-linux-gnu",
                                               .level      = codegen::opt_level::O2};
        codegen::optimizer_options opt_opts{.level = codegen::opt_level::O2};

        cmd::build_exe cmd{{
            .input_path  = src_file,
            .output_path = exe_file,
            .target_opts = target_opts,
            .opt_opts    = opt_opts,
        }};
        REQUIRE(cmd.execute());
        CHECK(std::filesystem::exists(exe_file));
        CHECK(std::filesystem::file_size(exe_file) > 0);
    }

    SECTION("Valid parameterless main function compiles and links executable") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_no_args_main.gh"};
        tempfile            exe_file{"test_no_args_main_out"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                pub const main := fn(): i32 {{
                    return 42;
                }};
            )");
        }

        cmd::build_exe cmd{{
            .input_path  = src_file,
            .output_path = exe_file,
        }};
        REQUIRE(cmd.execute());
        CHECK(std::filesystem::exists(exe_file));
        CHECK(std::filesystem::file_size(exe_file) > 0);
    }

    SECTION("Cross-target executable compilation for Linux x86_64 emits ELF binary") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_linux_exe_source.gh"};
        tempfile            exe_file{"test_linux_exe_output"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                pub const main := fn(args: [][:0]u8): void {{
                    return;
                }};
            )");
        }

        codegen::target_options target_opts{
            .triple_str = "x86_64-unknown-linux-gnu",
            .level      = codegen::opt_level::O2,
        };
        codegen::optimizer_options opt_opts{.level = codegen::opt_level::O2};

        cmd::build_exe cmd{{
            .input_path  = src_file,
            .output_path = exe_file,
            .target_opts = target_opts,
            .opt_opts    = opt_opts,
        }};
        REQUIRE(cmd.execute());
        CHECK(std::filesystem::exists(exe_file));
        CHECK(std::filesystem::file_size(exe_file) > 0);
        CHECK(bin_utils::check_elf_header(exe_file));
    }

    SECTION("Private main function (not pub) returns COMPILATION_FAILED") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_private_main.gh"};
        tempfile            exe_file{"test_private_main_out"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                const main := fn(args: [][:0]u8): void {{
                    return;
                }};
            )");
        }

        cmd::build_exe cmd{{
            .input_path  = src_file,
            .output_path = exe_file,
        }};
        CHECK(UNWRAP_ERR(cmd.execute()) == clap::error::COMPILATION_FAILED);
    }

    SECTION("Invalid main signature returns COMPILATION_FAILED") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_invalid_main.gh"};
        tempfile            exe_file{"test_invalid_main_out"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                pub const main := fn(): bool {{
                    return true;
                }};
            )");
        }

        cmd::build_exe cmd{{
            .input_path  = src_file,
            .output_path = exe_file,
        }};
        CHECK(UNWRAP_ERR(cmd.execute()) == clap::error::COMPILATION_FAILED);
    }

    SECTION("build_exe with extra precompiled objects, library search paths, and library flags") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_exe_extra_opts.gh"};
        tempfile            exe_file{"test_exe_extra_opts_output"};
        tempfile            extra_obj{"test_extra_helper.o"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                pub const main := fn(args: [][:0]u8): void {{
                    return;
                }};
            )");
        }

        // Create a dummy extra object file
        {
            std::ofstream out{extra_obj.path};
            fmt::print(out, "dummy object content");
        }

        cmd::build_exe cmd{{
            .input_path  = src_file,
            .output_path = exe_file,
            .target_opts =
                {
                    .triple_str = "x86_64-unknown-linux-gnu",
                    .level      = codegen::opt_level::O2,
                },
            .opt_opts      = {.level = codegen::opt_level::O2},
            .extra_objects = {extra_obj},
            .library_paths = {std::filesystem::path{"/usr/lib"}, std::filesystem::path{"/lib"}},
            .libraries     = {"m", "c"},
        }};
        CHECK(cmd.get_opts().extra_objects.size() == 1);
        CHECK(cmd.get_opts().library_paths.size() == 2);
        CHECK(cmd.get_opts().libraries.size() == 2);
    }
}

} // namespace ghoti::tests
