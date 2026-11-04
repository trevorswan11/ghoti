#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>
#include <fmt/ostream.h>
#include <stdx/types.hh>

#include "compiler/codegen/llvm_scope.hh"
#include "compiler/codegen/target.hh"
#include "driver/clap/error.hh"
#include "driver/cmd/build/test.hh"
#include "support/path_utils.hh"
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
                    let a = 10 + 20;
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

    SECTION("A user-defined `main` does not hijack the test runner entry point") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_driver_user_main.gh"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                pub const main = fn(): i32 {{
                    return 3;
                }};

                test "passes" {{
                    @expect(main() == 3);
                }}
            )");
        }

        cmd::test_cmd cmd{{
            .input_path = src_file,
        }};
        REQUIRE(cmd.execute());
    }

    SECTION("Source file with no test blocks compiles, runs, and exits cleanly") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_driver_no_tests.gh"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                pub const add = fn(a: i32, b: i32): i32 {{
                    return a + b;
                }};
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

    SECTION("A plain (non-test-wrapped) import does not discover the imported module's tests") {
        codegen::llvm_scope scope;
        tempfile            helper_file{"test_driver_helper_plain.gh"};
        tempfile            main_file{"test_driver_main_plain.gh"};

        {
            std::ofstream out{helper_file.path};
            fmt::print(out, R"(
                pub const helper_val = 42;

                test "imported module test" {{
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
                    @expect(helper.helper_val == 42);
                }}
            )",
                       helper_file.path.filename().string());
        }

        cmd::test_cmd cmd{{
            .input_path = main_file,
        }};
        REQUIRE(cmd.execute());
    }

    SECTION("Tests in a `test { import ... }`-nested module are discovered and executed") {
        codegen::llvm_scope scope;
        tempfile            helper_file{"test_driver_helper.gh"};
        tempfile            main_file{"test_driver_main.gh"};

        {
            std::ofstream out{helper_file.path};
            fmt::print(out, R"(
                pub const helper_val = 42;

                test "imported module test" {{
                    @expect(helper_val == 42);
                }}
            )");
        }

        {
            std::ofstream out{main_file.path};
            fmt::print(out,
                       R"(
                test {{
                    import "{}" as helper;
                }}

                test "main module test" {{
                    @expect(true);
                }}
            )",
                       helper_file.path.filename().string());
        }

        cmd::test_cmd cmd{{
            .input_path = main_file,
        }};
        REQUIRE(cmd.execute());
    }

    SECTION("Failing test in a `test { import ... }`-nested module fails test run") {
        codegen::llvm_scope scope;
        tempfile            helper_file{"test_driver_helper_fail.gh"};
        tempfile            main_file{"test_driver_main_pass.gh"};

        {
            std::ofstream out{helper_file.path};
            fmt::print(out, R"(
                pub const helper_val = 42;

                test "imported module failing test" {{
                    @require(false);
                }}
            )");
        }

        {
            std::ofstream out{main_file.path};
            fmt::print(out,
                       R"(
                test {{
                    import "{}" as helper;
                }}

                test "main module test" {{
                    @expect(true);
                }}
            )",
                       helper_file.path.filename().string());
        }

        cmd::test_cmd cmd{{
            .input_path = main_file,
        }};
        CHECK(!cmd.execute());
    }

    SECTION("An unaliased `test { import \"x.gh\"; }` import (no `as` at all) is discovered") {
        codegen::llvm_scope scope;
        tempfile            helper_file{"test_driver_helper_noalias.gh"};
        tempfile            main_file{"test_driver_main_noalias.gh"};

        {
            std::ofstream out{helper_file.path};
            fmt::print(out, R"(
                test "imported module failing test" {{
                    @require(false);
                }}
            )");
        }

        {
            std::ofstream out{main_file.path};
            fmt::print(out,
                       R"(
                test {{
                    import "{}";
                }}

                test "main module test" {{
                    @expect(true);
                }}
            )",
                       helper_file.path.filename().string());
        }

        cmd::test_cmd cmd{{
            .input_path = main_file,
        }};
        CHECK(!cmd.execute());
    }

    SECTION("A plain top-level import with a discardable (`as _`) alias is not discovered") {
        codegen::llvm_scope scope;
        tempfile            helper_file{"test_driver_helper_discard.gh"};
        tempfile            main_file{"test_driver_main_discard_plain.gh"};

        {
            std::ofstream out{helper_file.path};
            fmt::print(out, R"(
                test "imported module failing test" {{
                    @require(false);
                }}
            )");
        }

        {
            std::ofstream out{main_file.path};
            fmt::print(out,
                       R"(
                import "{}" as _;

                test "main module test" {{
                    @expect(true);
                }}
            )",
                       helper_file.path.filename().string());
        }

        cmd::test_cmd cmd{{
            .input_path = main_file,
        }};
        REQUIRE(cmd.execute());
    }

    SECTION("A `test { import \"x.gh\" as _; }` discardable-alias import is still discovered") {
        codegen::llvm_scope scope;
        tempfile            helper_file{"test_driver_helper_discard2.gh"};
        tempfile            main_file{"test_driver_main_discard_test.gh"};

        {
            std::ofstream out{helper_file.path};
            fmt::print(out, R"(
                test "imported module failing test" {{
                    @require(false);
                }}
            )");
        }

        {
            std::ofstream out{main_file.path};
            fmt::print(out,
                       R"(
                test {{
                    import "{}" as _;
                }}

                test "main module test" {{
                    @expect(true);
                }}
            )",
                       helper_file.path.filename().string());
        }

        cmd::test_cmd cmd{{
            .input_path = main_file,
        }};
        CHECK(!cmd.execute());
    }

    SECTION("A discardable-alias import binds no accessible symbol") {
        codegen::llvm_scope scope;
        tempfile            helper_file{"test_driver_helper_discard3.gh"};
        tempfile            main_file{"test_driver_main_discard_inaccessible.gh"};

        {
            std::ofstream out{helper_file.path};
            fmt::print(out, R"(
                pub const helper_val = 42;
            )");
        }

        {
            std::ofstream out{main_file.path};
            fmt::print(out,
                       R"(
                import "{}" as _;

                test "tries to use the discarded alias" {{
                    @expect(helper.helper_val == 42);
                }}
            )",
                       helper_file.path.filename().string());
        }

        cmd::test_cmd cmd{{.input_path = main_file}};
        CHECK(UNWRAP_ERR(cmd.execute()) == clap::error::COMPILATION_FAILED);
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

                pub const test_runner = fn(args: [][:0]u8, tests: []builtin.Test): i32 {{
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

                pub const test_runner = fn(args: [][:0]u8, tests: []builtin.Test): i32 {{
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

                pub const test_runner = fn(tests: []builtin.Test): i32 {{
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

                pub const test_runner = fn(args: [][:0]u8, tests: []builtin.Test): void {{
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

                pub const test_runner = fn(args: [][:0]u8, tests: []builtin.Test): i32 {{
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
        CHECK(path_utils::exists(out_path));
    }

    // `std.meta` folds the dependent return type, whose nodes live in this file and not in meta's
    SECTION("A dependent return type read through std.meta folds in its own module") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_driver_foreign_deferred.gh"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                import std;

                const widen = fn(comptime T: type, v: T): @Int(.{{
                    .signedness = .unsigned,
                    .bits = @typeInfo(T).int.bits + 1,
                }}) {{
                    const R = std.meta.return_type(@TypeOf(widen));
                    return @as(R, v);
                }};

                test "widen" {{
                    @expect(widen(u32, 1) == 1u33);
                    @expect(widen(u4, 9) == 9u5);
                }}
            )");
        }

        cmd::test_cmd cmd{{
            .input_path = src_file,
        }};
        REQUIRE(cmd.execute());
    }

    // The caller's own `T` must not leak into the callee's dependent return type
    SECTION("A self-referential dependent return type folds under a caller binding the same name") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_driver_self_deferred_shadow.gh"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                import std;
                const math = std.math;

                const Significand = fn(comptime T: type): type {{
                    const bits = math.ceil_power_of_two(u16, math.float_fractional_bits(T) + 1)!;
                    return @Int(.{{ .signedness = .unsigned, .bits = bits }});
                }};

                test "significand" {{
                    @expect(Significand(f32) == u32);
                    @expect(Significand(f64) == u64);
                    @expect(Significand(f16) == u16);
                }}
            )");
        }

        cmd::test_cmd cmd{{
            .input_path = src_file,
        }};
        REQUIRE(cmd.execute());
    }

    // Folding `Wrap(T)` while `Wrap`'s own parameter is retyped to a bound argument once rejected
    // the type argument `u8` as a number given to a struct parameter
    SECTION("A type built up in a `comptime let mut` initializes after other generic tests") {
        codegen::llvm_scope scope;
        tempfile            src_file{"test_driver_comptime_type_var.gh"};

        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                import std;

                const Counter = struct {{
                    n: u32,
                    limit: u32,
                    pub const next = fn(&mut self): std.Option(u32) {{
                        if (self.n >= self.limit) {{ return .empty; }}
                        self.n += 1;
                        return .of(self.n);
                    }};
                }};

                const first_even = fn(xs: []u32): std.Option(u32) {{
                    for (xs) |x| {{ if (x % 2 == 0) {{ return .of(x); }} }}
                    return .empty;
                }};

                const half_of_first_even = fn(xs: []u32): std.Option(u32) {{
                    let v = first_even(xs)?;
                    return .of(v / 2);
                }};

                const Wrap = fn(comptime T: type): type {{ return struct {{ inner: T }}; }};

                const fail_after = fn(log: &mut u32): std.Result(void, bool) {{
                    errdefer log += 10;
                    defer log += 1;
                    return .fail(true);
                }};

                test "while capture iterator" {{
                    let mut c = Counter{{ .n = 0, .limit = 3 }};
                    let mut total: u32 = 0;
                    while (c.next()) |v| {{ total += v; }}
                    @expect(total == 6);
                }}

                test "option" {{
                    let xs = [_]u32{{ 3, 8, 5 }};
                    @expect(half_of_first_even(xs[0..]).eq(@as(u32, 4)));
                    let ys = [_]u32{{ 1, 3 }};
                    @expect(half_of_first_even(ys[0..]) == .none);
                }}

                test "comptime let mut type building" {{
                    comptime let mut T: type = u8;
                    unroll for (0..3) |_| {{ T = Wrap(T); }}
                    let w: T = .{{ .inner = .{{ .inner = .{{ .inner = 5 }} }} }};
                    @expect(w.inner.inner.inner == 5);
                }}
            )");
        }

        cmd::test_cmd cmd{{
            .input_path = src_file,
        }};
        REQUIRE(cmd.execute());
    }
}

} // namespace ghoti::tests
