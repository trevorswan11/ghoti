#include <filesystem>
#include <fmt/base.h>
#include <fstream>

#include <catch2/catch_test_macros.hpp>
#include <fmt/ostream.h>

#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "driver/clap/error.hh"
#include "driver/cmd/build_obj.hh"
#include "support/bin_utils.hh"
#include "support/tempfile.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("build_obj command execution") {
    SECTION("Non-existent input file returns FILE_NOT_FOUND") {
        cmd::build_obj cmd{"non_existent_file_12345.gh", "out.o", {}, {}};
        CHECK(UNWRAP_ERR(cmd.execute()) == clap::error::FILE_NOT_FOUND);
    }

    SECTION("Valid source file compiles and emits object file") {
        tempfile src_file{"test_source.gh"};
        tempfile obj_file{"test_output.o"};

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

        cmd::build_obj cmd{src_file.path, obj_file.path, target_opts, opt_opts};
        REQUIRE(cmd.execute());
        CHECK(std::filesystem::exists(obj_file.path));
        CHECK(std::filesystem::file_size(obj_file.path) > 0);
    }

    SECTION("Cross-target compilation for Linux x86_64 emits ELF binary") {
        tempfile src_file{"test_linux_source.gh"};
        tempfile obj_file{"test_linux_output.o"};

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

        cmd::build_obj cmd{src_file.path, obj_file.path, target_opts, opt_opts};
        REQUIRE(cmd.execute());
        CHECK(std::filesystem::exists(obj_file.path));
        CHECK(std::filesystem::file_size(obj_file.path) > 0);
        CHECK(bin_utils::check_elf_header(obj_file.path));
    }

    SECTION("Invalid syntax returns COMPILATION_FAILED") {
        tempfile src_file{"test_invalid.gh"};
        tempfile obj_file{"test_invalid.o"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, "pub const invalid_syntax := ;;;");
        }

        cmd::build_obj cmd{src_file.path, obj_file.path, {}, {}};
        CHECK(UNWRAP_ERR(cmd.execute()) == clap::error::COMPILATION_FAILED);
    }
}

} // namespace ghoti::tests
