#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/base.h>
#include <fmt/ostream.h>
#include <gsl/util>

#include "compiler/codegen/llvm_scope.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "driver/clap/error.hh"
#include "driver/cmd/build_obj.hh"
#include "driver/cmd/build_options.hh"
#include "support/bin_utils.hh"
#include "support/tempfile.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("build_obj command execution") {
    SECTION("Non-existent input file returns FILE_NOT_FOUND") {
        codegen::llvm_scope scope;
        cmd::build_obj      cmd{{
                 .input_path  = "non_existent_file_12345.gh",
                 .output_path = "out.o",
        }};
        CHECK(UNWRAP_ERR(cmd.execute()) == clap::error::FILE_NOT_FOUND);
    }

    SECTION("Valid source file compiles and emits object file") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_source.gh"};
        tempfile            obj_file{"test_output.o"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                pub const add := fn(a: i64, b: i64): i64 {{
                    return a + b;
                }};

                pub const main := fn(): i64 {{
                    return add(40l, 2l);
                }};
            )");
        }

        codegen::target_options    target_opts{.level = codegen::opt_level::O2};
        codegen::optimizer_options opt_opts{.level = codegen::opt_level::O2};

        cmd::build_obj cmd{{
            .input_path  = src_file,
            .output_path = obj_file,
            .target_opts = target_opts,
            .opt_opts    = opt_opts,
        }};
        REQUIRE(cmd.execute());
        CHECK(std::filesystem::exists(obj_file));
        CHECK(std::filesystem::file_size(obj_file) > 0);
    }

    SECTION("Cross-target compilation for Linux x86_64 emits ELF binary") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_linux_source.gh"};
        tempfile            obj_file{"test_linux_output.o"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                pub const square := fn(x: i64): i64 {{
                    return x * x;
                }};
            )");
        }

        codegen::target_options target_opts{
            .triple_str = "x86_64-unknown-linux-gnu",
            .level      = codegen::opt_level::O2,
        };
        codegen::optimizer_options opt_opts{.level = codegen::opt_level::O2};

        cmd::build_obj cmd{{
            .input_path  = src_file,
            .output_path = obj_file,
            .target_opts = target_opts,
            .opt_opts    = opt_opts,
        }};
        REQUIRE(cmd.execute());
        CHECK(std::filesystem::exists(obj_file));
        CHECK(std::filesystem::file_size(obj_file) > 0);
        CHECK(bin_utils::check_elf_header(obj_file));
    }

    SECTION("Invalid syntax returns COMPILATION_FAILED") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_invalid.gh"};
        tempfile            obj_file{"test_invalid.o"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, "pub const invalid_syntax := ;;;");
        }

        cmd::build_obj cmd{{.input_path = src_file, .output_path = obj_file}};
        CHECK(UNWRAP_ERR(cmd.execute()) == clap::error::COMPILATION_FAILED);
    }

    SECTION("Multi-file compilation with relative file import on disk") {
        codegen::llvm_scope scope;
        // Place helper in the same directory as main
        const auto parent_dir{std::filesystem::temp_directory_path()};
        tempfile   helper_path{std::in_place, parent_dir / "ghoti_test_helper.gh"};
        tempfile   main_path{std::in_place, parent_dir / "ghoti_test_main.gh"};

        tempfile obj_file{"test_multi_out.o"};
        {
            std::ofstream helper_out{helper_path.path};
            fmt::print(helper_out, R"(
                pub const multiply := fn(a: i64, b: i64): i64 {{
                    return a * b;
                }};
            )");

            std::ofstream main_out{main_path.path};
            fmt::print(main_out, R"(
                pub import "ghoti_test_helper.gh" as helper;

                pub const calc := fn(x: i64): i64 {{
                    return helper::multiply(x, 2l);
                }};
            )");
        }

        cmd::build_obj cmd{{.input_path = main_path, .output_path = obj_file}};
        REQUIRE(cmd.execute());
        CHECK(std::filesystem::exists(obj_file));
        CHECK(std::filesystem::file_size(obj_file) > 0);
    }

    SECTION("Standard library import on disk with generic functions") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_std_import.gh"};
        tempfile            obj_file{"test_std_output.o"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                pub import std;

                pub const pick_min := fn(a: i64, b: i64): i64 {{
                    return std::min(a, b);
                }};

                pub const pick_max := fn(a: i64, b: i64): i64 {{
                    return std::max(a, b);
                }};
            )");
        }

        cmd::build_obj cmd{{.input_path = src_file, .output_path = obj_file}};
        REQUIRE(cmd.execute());
        CHECK(std::filesystem::exists(obj_file));
        CHECK(std::filesystem::file_size(obj_file) > 0);
    }

    SECTION("Custom library module import via -m on disk") {
        codegen::llvm_scope scope;
        tempfile            custom_lib{"test_custom_lib.gh"};
        tempfile            src_file{"test_custom_import.gh"};
        tempfile            obj_file{"test_custom_output.o"};

        {
            std::ofstream lib_out{custom_lib.path};
            fmt::print(lib_out, R"(
                pub const custom_fn := fn(x: i64): i64 {{
                    return x + 100l;
                }};
            )");

            std::ofstream src_out{src_file.path};
            fmt::print(src_out, R"(
                pub import mylib;

                pub const run_custom := fn(v: i64): i64 {{
                    return mylib::custom_fn(v);
                }};
            )");
        }

        std::vector<cmd::module_binding> modules{{"mylib", custom_lib}};
        cmd::build_obj                   cmd{{
                              .input_path  = src_file,
                              .output_path = obj_file,
                              .modules     = std::move(modules),
        }};
        REQUIRE(cmd.execute());
        CHECK(std::filesystem::exists(obj_file));
        CHECK(std::filesystem::file_size(obj_file) > 0);
    }

    SECTION("Custom generic library module import via -m on disk") {
        codegen::llvm_scope scope;
        tempfile            custom_lib{"test_custom_generic_lib.gh"};
        tempfile            src_file{"test_custom_generic_import.gh"};
        tempfile            obj_file{"test_custom_generic_output.o"};

        {
            std::ofstream lib_out{custom_lib.path};
            fmt::print(lib_out, R"(
                pub const clamp := fn(val: auto, low: auto, high: auto): auto {{
                    if (val < low) {{
                        return low;
                    }}
                    if (val > high) {{
                        return high;
                    }}
                    return val;
                }};
            )");

            std::ofstream src_out{src_file.path};
            fmt::print(src_out, R"(
                pub import math;

                pub const clamp_int := fn(v: i64): i64 {{
                    return math::clamp(v, 0l, 100l);
                }};
            )");
        }

        std::vector<cmd::module_binding> modules{{"math", custom_lib}};
        cmd::build_obj                   cmd{{
                              .input_path  = src_file,
                              .output_path = obj_file,
                              .modules     = std::move(modules),
        }};
        REQUIRE(cmd.execute());
        CHECK(std::filesystem::exists(obj_file));
        CHECK(std::filesystem::file_size(obj_file) > 0);
    }
}

} // namespace ghoti::tests
