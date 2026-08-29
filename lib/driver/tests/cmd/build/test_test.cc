#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>
#include <fmt/ostream.h>
#include <stdx/types.hh>

#include "compiler/codegen/llvm_scope.hh"
#include "driver/clap/error.hh"
#include "driver/cmd/build/test.hh"
#include "support/tempfile.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("test command execution") {
    SECTION("Non-existent input file returns FILE_NOT_FOUND") {
        codegen::llvm_scope scope;
        cmd::test_cmd       cmd{{
                  .input_path = "non_existent_file_12345.gh",
        }};
        CHECK(UNWRAP_ERR(cmd.execute()) == clap::error::FILE_NOT_FOUND);
    }

    SECTION("Valid source file with passing test compiles and runs cleanly") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_driver_pass.gh"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                test "simple passing test" {{
                    const a := 10 + 20;
                    @expect(a == 30);
                    @require(a > 0);
                }}
            )");
        }

        cmd::test_cmd cmd{{
            .input_path = src_file,
        }};
        REQUIRE(cmd.execute());
    }

    SECTION("Test file with failing assertion returns non-zero exit code") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_driver_fail.gh"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                test "failing require test" {{
                    @require(1 == 2);
                }}
            )");
        }

        cmd::test_cmd cmd{{
            .input_path = src_file,
        }};
        CHECK(!cmd.execute());
    }

    SECTION("Tests in imported modules are discovered and executed") {
        codegen::llvm_scope scope;
        tempfile            helper_file{"test_driver_helper.gh"};
        tempfile            main_file{"test_driver_main.gh"};

        {
            std::ofstream out{helper_file.path};
            fmt::print(out, R"(
                pub const helper_val := 42;

                test "imported module test" {{
                    @expect(helper_val == 42);
                }}
            )");
        }

        {
            std::ofstream out{main_file.path};
            fmt::print(out,
                       R"(
                import "{}" as helper;

                test "main module test" {{
                    @expect(helper::helper_val == 42);
                }}
            )",
                       helper_file.path.filename().string());
        }

        cmd::test_cmd cmd{{
            .input_path = main_file,
        }};
        REQUIRE(cmd.execute());
    }

    SECTION("Failing test in imported module fails test run") {
        codegen::llvm_scope scope;
        tempfile            helper_file{"test_driver_helper_fail.gh"};
        tempfile            main_file{"test_driver_main_pass.gh"};

        {
            std::ofstream out{helper_file.path};
            fmt::print(out, R"(
                pub const helper_val := 42;

                test "imported module failing test" {{
                    @require(false);
                }}
            )");
        }

        {
            std::ofstream out{main_file.path};
            fmt::print(out,
                       R"(
                import "{}" as helper;

                test "main module test" {{
                    @expect(helper::helper_val == 42);
                }}
            )",
                       helper_file.path.filename().string());
        }

        cmd::test_cmd cmd{{
            .input_path = main_file,
        }};
        CHECK(!cmd.execute());
    }

    SECTION("A non-weak `test_runner` overrides the builtin default and passes") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_driver_custom_runner_pass.gh"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                test "sample test" {{
                    @require(false);
                }}

                pub const test_runner := fn(tests: []builtin::Test): i32 {{
                    _ = tests;
                    return 0;
                }};
            )");
        }

        cmd::test_cmd cmd{{
            .input_path = src_file,
        }};
        REQUIRE(cmd.execute());
    }

    SECTION("An overriding `test_runner` returning non-zero fails test_cmd execution") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_driver_custom_runner_fail.gh"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                test "sample test" {{
                    @expect(true);
                }}

                pub const test_runner := fn(tests: []builtin::Test): i32 {{
                    _ = tests;
                    return 12;
                }};
            )");
        }

        cmd::test_cmd cmd{{
            .input_path = src_file,
        }};
        CHECK(!cmd.execute());
    }
}

} // namespace ghoti::tests
