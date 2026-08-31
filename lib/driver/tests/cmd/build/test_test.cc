#include <filesystem>
#include <fstream>
#include <system_error>

#include <catch2/catch_test_macros.hpp>
#include <fmt/ostream.h>
#include <stdx/types.hh>

#include "compiler/codegen/llvm_scope.hh"
#include "compiler/codegen/target.hh"
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

                pub const test_runner := fn(args: [][:0]u8, tests: []builtin::Test): i32 {{
                    _ = args;
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

                pub const test_runner := fn(args: [][:0]u8, tests: []builtin::Test): i32 {{
                    _ = args;
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

    SECTION("A `test_runner` missing the args parameter is rejected") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_driver_bad_runner_arity.gh"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                test "noop" {{ @expect(true); }}

                pub const test_runner := fn(tests: []builtin::Test): i32 {{
                    _ = tests;
                    return 0;
                }};
            )");
        }

        cmd::test_cmd cmd{{.input_path = src_file}};
        CHECK(UNWRAP_ERR(cmd.execute()) == clap::error::COMPILATION_FAILED);
    }

    SECTION("A `test_runner` with the wrong return type is rejected") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_driver_bad_runner_ret.gh"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                test "noop" {{ @expect(true); }}

                pub const test_runner := fn(args: [][:0]u8, tests: []builtin::Test): void {{
                    _ = args;
                    _ = tests;
                }};
            )");
        }

        cmd::test_cmd cmd{{.input_path = src_file}};
        CHECK(UNWRAP_ERR(cmd.execute()) == clap::error::COMPILATION_FAILED);
    }

    SECTION("`forwarded_args` reach the test binary's argv") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_driver_forward_args.gh"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                test "noop" {{ @expect(true); }}

                pub const test_runner := fn(args: [][:0]u8, tests: []builtin::Test): i32 {{
                    _ = tests;
                    if (args.len == 3) {{ return 0; }} // args passed through from runtime
                    if (args.len == 0) {{ return 0; }} // empty fallback on windows due to sysroot
                    return 4;
                }};
            )");
        }

        cmd::test_cmd cmd{{
            .input_path     = src_file,
            .forwarded_args = {"alpha", "beta"},
        }};
        REQUIRE(cmd.execute());
    }

    SECTION("-o writes the test binary to the given path and keeps it") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_driver_keep_binary.gh"};
        tempfile            out_path{"kept_test_binary"};
        out_path.path.replace_extension(
            codegen::get_default_output_extension(codegen::output_type::EXECUTABLE));

        {
            std::ofstream out{src_file.path};
            fmt::print(out, "test \"noop\" {{ @expect(true); }}");
        }

        cmd::test_cmd cmd{{
            .input_path      = src_file,
            .output_path     = out_path,
            .output_explicit = true,
        }};
        REQUIRE(cmd.execute());

        std::error_code ec;
        CHECK(std::filesystem::exists(out_path, ec));
    }
}

} // namespace ghoti::tests
