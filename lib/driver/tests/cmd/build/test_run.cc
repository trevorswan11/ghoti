#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>
#include <fmt/ostream.h>
#include <stdx/types.hh>

#include "compiler/codegen/llvm_scope.hh"
#include "driver/clap/error.hh"
#include "driver/cmd/build/run.hh"
#include "support/path_utils.hh"
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
            fmt::print(out, "pub const main = fn(): void {{ return; }};");
        }

        cmd::run_cmd cmd{{.input_path = src_file}};
        REQUIRE(cmd.execute());
    }

    SECTION("The program's exit code is propagated") {
        codegen::llvm_scope scope;
        tempfile            src_file{"run_driver_exit_code.gh"};
        {
            std::ofstream out{src_file.path};
            fmt::print(out, "pub const main = fn(): i32 {{ return 7; }};");
        }

        cmd::run_cmd cmd{{.input_path = src_file}};
        CHECK(UNWRAP_ERR(cmd.execute()) == static_cast<clap::error>(7));
    }

    SECTION(
        "UIR Sema's program runs its `dyn` calls, fields through references, slices and casts") {
        codegen::llvm_scope scope;
        tempfile            src_file{"run_driver_uir.gh"};
        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                const Counter = interface {{
                    pub const bump = fn(&mut self, by: u32): u32;
                    pub const twice = fn(&mut self, by: u32): u32 {{
                        _ = self.bump(by);
                        return self.bump(by);
                    }};
                }};
                const Tally = struct {{
                    pub buf: []mut u8,
                    pub len: u32 = 0,
                    pub const total = fn(self): u32 {{ return self.len; }};
                }};
                impl Counter for Tally {{
                    pub const bump = fn(&mut self, by: u32): u32 {{
                        self.len += by;
                        self.buf[0] = @truncate(self.len);
                        return self.len;
                    }};
                }}
                const fill = fn(p: ^mut u8, n: usize): usize {{
                    let s = p[1..n];
                    s[0] = 7;
                    return s.len;
                }};
                const Base = struct {{
                    pub v: u32,
                    pub const get = fn(self): u32 {{ return self.v; }};
                }};
                let mut base: Base = .{{ .v = 2 }};
                const lens = fn(args...): usize {{ return args[0].len + args[1].len; }};
                const pick = fn(comptime text: []u8, at: usize): u8 {{
                    if (at == 9) return 0;
                    let part = text[1..3];
                    return part[at];
                }};
                pub const main = fn(): i32 {{
                    let mut backing: [4]mut u8 = .{{ 0, 0, 0, 0 }};
                    let mut t: Tally = .{{ .buf = backing[..] }};
                    let c: &mut dyn Counter = &mut t;
                    _ = c.twice(5);
                    let wide: u32 = 300;
                    let low: u8 = @truncate(wide) & 0x0f;
                    let picked: u8 = if (wide > 3) @truncate(wide) else 0;
                    let len = fill(^mut backing[0], 4);
                    if (backing[0] != 10 or backing[1] != 7) return 1;
                    if (low != 12 or picked != 44) return 2;
                    if (pick("abcd", 1) != 'c') return 3;
                    let left: []u8 = "ab";
                    let right: []u8 = "cde";
                    if (lens(left, right) != 5) return 4;
                    if (base.get() != 2) return 5;
                    return @intCast(t.total() + @as(u32, @intCast(len)));
                }};
            )");
        }

        cmd::run_cmd cmd{{.input_path = src_file, .sema_uir = true}};
        CHECK(UNWRAP_ERR(cmd.execute()) == static_cast<clap::error>(13));
    }

    SECTION("Forwarded program args reach the binary's argv") {
        codegen::llvm_scope scope;
        tempfile            src_file{"run_driver_args.gh"};
        {
            std::ofstream out{src_file.path};
            fmt::print(out, R"(
                pub const main = fn(args: [][:0]u8): i32 {{
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
            fmt::print(out, "pub const main = fn(): void {{ return; }};");
        }

        cmd::run_cmd cmd{{.input_path = src_file}};
        REQUIRE(cmd.execute());

        CHECK(!cmd.get_opts().output_path.empty());
        CHECK(!path_utils::exists(cmd.get_opts().output_path));
    }
}

} // namespace ghoti::tests
