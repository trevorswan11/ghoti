#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/ostream.h>

#include "compiler/codegen/llvm_scope.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "driver/clap/error.hh"
#include "driver/cmd/build/executable.hh"
#include "driver/cmd/build/options.hh"
#include "support/bin_utils.hh"
#include "support/path_utils.hh"
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
                pub const main = fn(args: [][:0]u8): void {{
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
        CHECK(path_utils::exists(exe_file));
        CHECK(std::filesystem::file_size(exe_file) > 0);
    }

    SECTION("UIR Sema builds an executable through the entry code") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_exe_uir.gh"};
        tempfile            exe_file{"test_exe_uir_out"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                const fib = fn(n: i32): i32 {{ return if (n < 2) n else fib(n - 1) + fib(n - 2); }};
                pub const main = fn(): i32 {{ return fib(10); }};
            )");
        }

        cmd::build_exe cmd{{
            .input_path  = src_file,
            .output_path = exe_file,
            .sema_uir    = true,
        }};
        REQUIRE(cmd.execute());
        CHECK(path_utils::exists(exe_file));
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
                pub const main = fn(args: [][:0]u8): i32 {{
                    return @intCast(i32, args.len);
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
        CHECK(path_utils::exists(exe_file));
        CHECK(std::filesystem::file_size(exe_file) > 0);
    }

    SECTION("Valid parameterless main function compiles and links executable") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_no_args_main.gh"};
        tempfile            exe_file{"test_no_args_main_out"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                pub const main = fn(): i32 {{
                    return 42;
                }};
            )");
        }

        cmd::build_exe cmd{{
            .input_path  = src_file,
            .output_path = exe_file,
            .target_opts =
                {
                    .triple_str = "x86_64-unknown-linux-gnu",
                    .level      = codegen::opt_level::O2,
                },
        }};
        REQUIRE(cmd.execute());
        CHECK(path_utils::exists(exe_file));
        CHECK(std::filesystem::file_size(exe_file) > 0);
    }

    SECTION("Cross-target executable compilation for Linux x86_64 emits ELF binary") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_linux_exe_source.gh"};
        tempfile            exe_file{"test_linux_exe_output"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                pub const main = fn(args: [][:0]u8): void {{
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
        CHECK(path_utils::exists(exe_file));
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
                const main = fn(args: [][:0]u8): void {{
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
                pub const main = fn(): bool {{
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

    SECTION("Syntax error in a non-first top-level declaration returns COMPILATION_FAILED") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_syntax_error_after_decl.gh"};
        tempfile            exe_file{"test_syntax_error_after_decl_out"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                const X = 1;

                pub const main = fn(): i32 {{
                    return 1 +;
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
                pub const main = fn(args: [][:0]u8): void {{
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

TEST_CASE("build_exe analyzes only what main reaches unless --check-all is given") {
    codegen::llvm_scope scope;
    tempfile            src_file{"test_exe_lazy.gh"};
    tempfile            exe_file{"test_exe_lazy_out"};

    {
        std::ofstream out{src_file.path};
        fmt::print(out, R"(
            const unused = fn(): i32 {{ return true; }};
            pub const main = fn(): i32 {{ return 0; }};
        )");
    }

    SECTION("An unused declaration's error doesn't stop the build") {
        std::ostringstream errors;
        cmd::build_exe     cmd{{.input_path = src_file, .output_path = exe_file}, errors};
        REQUIRE(cmd.execute());
        CHECK(errors.view().empty());
    }

    SECTION("--check-all reports it") {
        std::ostringstream errors;
        cmd::build_exe     cmd{{.input_path = src_file, .output_path = exe_file, .check_all = true},
                               errors};
        CHECK(UNWRAP_ERR(cmd.execute()) == clap::error::COMPILATION_FAILED);
        CHECK(errors.view().contains("TYPE_MISMATCH"));
    }
}

TEST_CASE("build_exe reports an error in what main reaches") {
    codegen::llvm_scope scope;
    tempfile            src_file{"test_exe_lazy_used.gh"};
    tempfile            exe_file{"test_exe_lazy_used_out"};

    {
        std::ofstream out{src_file.path};
        fmt::print(out, R"(
            const helper = fn(): i32 {{ return true; }};
            const middle = fn(): i32 {{ return helper(); }};
            pub const main = fn(): i32 {{ return middle(); }};
        )");
    }

    std::ostringstream errors;
    cmd::build_exe     cmd{{.input_path = src_file, .output_path = exe_file}, errors};
    CHECK(UNWRAP_ERR(cmd.execute()) == clap::error::COMPILATION_FAILED);
    CHECK(errors.view().contains("TYPE_MISMATCH"));
}

} // namespace ghoti::tests
