#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fmt/format.h>
#include <gsl/util>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <stdx/memory.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "catch2/catch_message.hpp"
#include "compiler/codegen/error.hh"
#include "compiler/codegen/llvm_scope.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "compiler/module/file_loader.hh"
#include "compiler/module/module.hh"
#include "compiler/module/stdlib.hh"
#include "compiler/sema/analyzer.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/bin_utils.hh"
#include "support/env.hh"
#include "support/path_utils.hh"
#include "support/subprocess.hh"
#include "support/tempfile.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

// Pulls the inline-asm template string out of a naked `_start`
[[nodiscard]] auto start_asm_text(llvm::Function& start_fn) -> std::string {
    REQUIRE_FALSE(start_fn.empty());
    auto& call{UNWRAP(llvm::dyn_cast<llvm::CallInst>(&start_fn.getEntryBlock().front()))};
    auto& inline_asm{UNWRAP(llvm::dyn_cast<llvm::InlineAsm>(call.getCalledOperand()))};
    return std::string{inline_asm.getAsmString()};
}

struct lowered {
    helpers::ctx_idx_pair   ctx;
    stdx::box<llvm::Module> mod;
};

// Analyzes `source` for `triple_str`, where the entry code is chosen, and lowers it there
[[nodiscard]] auto lower_for_triple(std::string_view   source,
                                    llvm::LLVMContext& context,
                                    std::string_view   triple_str) -> lowered {
    auto                       ctx{helpers::resolve_for_target(source, triple_str)};
    codegen::target_options    target_opts{.triple_str = std::string{triple_str}};
    auto                       machine{UNWRAP(codegen::create_target_machine(target_opts))};
    codegen::optimizer_options opt_opts{.target_machine = machine.get()};
    auto mod{UNWRAP(helpers::emit_llvm_ir_executable(*ctx.first, context, opt_opts))};
    return {std::move(ctx), std::move(mod)};
}

// Lowers for `triple_str` and returns the diagnostic error kind, expecting failure.
[[nodiscard]] auto lower_error_for_triple(helpers::sema_test_context& ctx,
                                          llvm::LLVMContext&          context,
                                          std::string_view triple_str) -> codegen::error {
    codegen::target_options    target_opts{.triple_str = std::string{triple_str}};
    auto                       machine{UNWRAP(codegen::create_target_machine(target_opts))};
    codegen::optimizer_options opt_opts{.target_machine = machine.get()};
    return UNWRAP_ERR(helpers::emit_llvm_ir_executable(ctx, context, opt_opts)).get_error();
}

// An error reported in a program's root, at the line it points to
struct root_error {
    usize       line{};
    std::string message;
};

// Analyzes `source` as a program's root, or a test build's, giving every error reported in it
[[nodiscard]] auto entry_errors(std::string_view source, bool testing = false)
    -> std::vector<root_error> {
    const tempdir dir{"entry_error"};
    dir.write("main.gh", source);

    std::ostringstream  errors;
    mod::file_loader    loader;
    mod::module_manager manager{loader};
    sema::analyzer      analyzer{manager, errors, false};
    analyzer.set_entry_point(true);
    const auto analyzed{analyzer.analyze(dir.path / "main.gh", testing)};
    INFO(errors.str());
    CHECK(analyzed);

    std::vector<root_error> found;
    const auto              root{UNWRAP(manager.try_get_file_module(dir.path / "main.gh"))};
    if (const auto diags{root->diagnostics.as_opt<sema::diagnostics>()}) {
        for (const auto& diag : *diags) {
            found.emplace_back<root_error>(
                {UNWRAP(diag.to_formattable().location).line, UNWRAP(diag.get_message())});
        }
    }
    return found;
}

// The one error analyzing `source` reports
[[nodiscard]] auto entry_error(std::string_view source) -> root_error {
    auto found{entry_errors(source)};
    REQUIRE(found.size() == 1);
    return std::move(found.front());
}

// The 0-based line `source` declares `main` on
[[nodiscard]] auto main_line(std::string_view source) -> usize {
    const auto at{source.find("pub const main")};
    REQUIRE(at != std::string_view::npos);
    return static_cast<usize>(std::ranges::count(source.substr(0, at), '\n'));
}

// A `Result` like `std.Result`, which a test program can't import
constexpr std::string_view result_type{R"(
const Res = fn(comptime T: type, comptime E: type): type {
    return union { ok: T, err: E };
};
impl(T: type, E: type) builtin.Unwrappable for Res(T, E) {
    const Output = T;
    const Residual = E;
    pub const branch = fn(&mut? self): builtin.Flow(&mut? T, E) {
        return match (self) {
            .ok => |&mut? v| .{ .continue = v },
            .err => |e| .{ .break = e },
        };
    };
}
)"};

} // namespace

TEST_CASE("Main function entry validation") {
    SECTION("Valid main entry with fn(args: [][:0]u8): void passes") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            pub const main = fn(args: [][:0]u8): void {
                return;
            };
        )")};
        CHECK(ctx->analyzer.validate_main_entry(ctx->root_mod));
    }

    SECTION("Missing main function returns error") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            pub const not_main = fn(args: [][:0]u8): void {
                return;
            };
        )")};
        CHECK_FALSE(ctx->analyzer.validate_main_entry(ctx->root_mod));
    }

    SECTION("Private main function (not pub) returns error") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const main = fn(args: [][:0]u8): void {
                return;
            };
        )")};
        CHECK_FALSE(ctx->analyzer.validate_main_entry(ctx->root_mod));
    }

    SECTION("Main with 0 params is valid") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            pub const main = fn(): void {
                return;
            };
        )")};
        CHECK(ctx->analyzer.validate_main_entry(ctx->root_mod));
    }

    SECTION("Main with 0 params and i32 return is valid") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            pub const main = fn(): i32 {
                return 0;
            };
        )")};
        CHECK(ctx->analyzer.validate_main_entry(ctx->root_mod));
    }

    SECTION("Main with i32 return is valid") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            pub const main = fn(args: [][:0]u8): i32 {
                return 0;
            };
        )")};
        CHECK(ctx->analyzer.validate_main_entry(ctx->root_mod));
    }
}

TEST_CASE("std/start.gh rejects a main it can't call") {
    const auto [source,
                message]{GENERATE(Catch::Generators::table<std::string_view, std::string_view>({
        {"pub const main = fn(args: [][:0]u8): bool { return true; };",
         "'main' must return 'void', 'noreturn', a 'builtin.ExitCode', or a 'Result' of them"},
        {"pub const main = fn(args: []u8): void {};",
         "'main' must take no parameters or one of type '[][:0]u8'"},
        {"pub const main = fn(a: [][:0]u8, b: i32): void {};",
         "'main' must take no parameters or one of type '[][:0]u8'"},
    }))};
    CAPTURE(source);
    const auto error{entry_error(source)};
    CHECK(error.line == 0);
    CHECK(error.message.starts_with(message));
}

TEST_CASE("main returns anything implementing builtin.ExitCode") {
    const auto [source, code]{GENERATE(Catch::Generators::table<std::string_view, u32>({
        {"pub const main = fn(): u8 { return 9; };", 9},
        {"pub const main = fn(): i64 { return 0x1_0000_000A; };", 10},
        {R"(
            const Code = enum { ok, bad };
            impl builtin.ExitCode for Code {
                pub const to_exit_code = fn(&self): i32 { return if (*self == .bad) 11 else 0; };
            }
            pub const main = fn(): Code { return .bad; };
        )",
         11},
    }))};
    CAPTURE(source);
    CHECK(helpers::compile_and_run(source) == code);
}

TEST_CASE("main returns a Result that exits with its value's or its error's code") {
    // An error whose code is 0 exits 0: the program's own word for what failure means
    const auto [main_fn, code]{GENERATE(Catch::Generators::table<std::string_view, u32>({
        {"pub const main = fn(): Res(void, u8) { return .{ .ok = {} }; };", 0},
        {"pub const main = fn(): Res(void, u8) { return .{ .err = 12 }; };", 12},
        {"pub const main = fn(): Res(i32, u8) { return .{ .ok = 13 }; };", 13},
        {"pub const main = fn(): Res(i32, u8) { return .{ .err = 14 }; };", 14},
        {"pub const main = fn(): Res(void, u8) { return .{ .err = 0 }; };", 0},
    }))};
    CAPTURE(main_fn);
    CHECK(helpers::compile_and_run(fmt::format("{}\n{}", result_type, main_fn)) == code);
}

TEST_CASE("main may never return") {
    // Only Windows has an exit call without libc; elsewhere it is enough that it compiles
    constexpr std::string_view source{R"(
        extern("kernel32", "ExitProcess") const ExitProcess: extern fn(
            code: u32,
        ) callconv(.winapi): noreturn;
        pub const main = fn(): noreturn { ExitProcess(16); };
    )"};
    CHECK(entry_errors(source).empty());
#if GHOTI_WINDOWS
    CHECK(helpers::compile_and_run(source) == 16);
#endif
}

TEST_CASE("std/start.gh rejects a Result main can't exit with") {
    const auto [main_fn,
                message]{GENERATE(Catch::Generators::table<std::string_view, std::string_view>({
        {"pub const main = fn(): Res(void, bool) { return .{ .ok = {} }; };",
         "the error 'main' returns must implement 'builtin.ExitCode'"},
        {"pub const main = fn(): Res(bool, u8) { return .{ .ok = true }; };",
         "the value 'main' returns in a 'Result' must be 'void' or implement "
         "'builtin.ExitCode'"},
    }))};
    CAPTURE(main_fn);
    const auto source{fmt::format("{}\n{}", result_type, main_fn)};
    const auto error{entry_error(source)};
    CHECK(error.line == main_line(source));
    CHECK(error.message.starts_with(message));
}

TEST_CASE("std/start.gh rejects a test_runner it can't call") {
    constexpr std::string_view source{R"(
        test "a" { @expect(true); }
        pub const test_runner = fn(args: [][:0]u8): i32 { _ = args; return 0; };
    )"};
    const auto                 errors{entry_errors(source, true)};
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().line == 2);
    CHECK(errors.front().message.starts_with(
        "'test_runner' must have signature 'fn(args: [][:0]u8, tests: []Test): i32'"));
}

TEST_CASE("An executable enters through std/start.gh, which calls main") {
    codegen::llvm_scope scope;
    llvm::LLVMContext   context;

    auto [ctx, llvm_mod]{lower_for_triple(R"(
        pub const main = fn(args: [][:0]u8): void {
            return;
        };
    )",
                                          context,
                                          "x86_64-apple-macosx")};

    std::string              err_str;
    llvm::raw_string_ostream os{err_str};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod, &os));

    // The program's `main` is renamed so the entry code's C `main(argc, argv)` can take the name
    auto& ghoti_main_fn{UNWRAP(llvm_mod->getFunction("_ghoti_main"))};
    CHECK(ghoti_main_fn.getReturnType()->isVoidTy());
    // SysV passes the `args` slice as its pointer and length
    REQUIRE(ghoti_main_fn.arg_size() == 2);
    CHECK(ghoti_main_fn.getArg(0)->getType()->isPointerTy());
    CHECK(ghoti_main_fn.getArg(1)->getType()->isIntegerTy(64));

    auto& c_main_fn{UNWRAP(llvm_mod->getFunction("main"))};
    CHECK(c_main_fn.getReturnType()->isIntegerTy(32));
    CHECK(c_main_fn.arg_size() == 2);
    CHECK(c_main_fn.getArg(0)->getType()->isIntegerTy(32));
    CHECK(c_main_fn.getArg(1)->getType()->isPointerTy());
}

TEST_CASE("Windows entry wrapper with an args parameter emits valid, verifiable IR") {
    // Object emission alone needs no real sysroot; linking is covered in test_linker.cc.
    codegen::llvm_scope scope;
    llvm::LLVMContext   context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const main = fn(args: [][:0]u8): i32 {
            return @as(i32, args.len);
        };
    )")};
    REQUIRE(ctx->analyzer.validate_main_entry(ctx->root_mod));

    tempfile                out_file{"test_windows_args_obj"};
    codegen::target_options target_opts{.triple_str = "x86_64-w64-windows-gnu"};
    CHECK(helpers::emit_object(*ctx, context, out_file, target_opts));
    CHECK(path_utils::exists(out_file));
}

TEST_CASE("Parameterless main function lowering") {
    codegen::llvm_scope scope;
    llvm::LLVMContext   context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const main = fn(): i32 {
            return 42;
        };
    )")};
    REQUIRE(ctx->analyzer.validate_main_entry(ctx->root_mod));
    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir_executable(*ctx, context))};

    std::string              err_str;
    llvm::raw_string_ostream os{err_str};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod, &os));

    auto& ghoti_main_fn{UNWRAP(llvm_mod->getFunction("_ghoti_main"))};
    CHECK(ghoti_main_fn.getReturnType()->isIntegerTy(32));
    CHECK(ghoti_main_fn.arg_size() == 0);
}

TEST_CASE("Linux executables get a freestanding _start that calls main and exits") {
    codegen::llvm_scope scope;
    llvm::LLVMContext   context;

    struct arch_case {
        std::string_view triple;
        std::string_view argc_load;
        std::string_view call_main;
        std::string_view exit_nr;
        std::string_view syscall_insn;
    };
    const auto tc{GENERATE(
        arch_case{"x86_64-unknown-linux-gnu",
                  "(%rsp), %rdi",
                  "call __ghoti_linux_main",
                  "$$231, %eax",
                  "syscall"},
        arch_case{
            "aarch64-unknown-linux-gnu", "ldr x0, [sp]", "bl __ghoti_linux_main", "#94", "svc #0"},
        arch_case{"riscv64-unknown-linux-gnu",
                  "ld a0, 0(sp)",
                  "call __ghoti_linux_main",
                  "li a7, 94",
                  "ecall"},
        arch_case{"riscv32-unknown-linux-gnu",
                  "lw a0, 0(sp)",
                  "call __ghoti_linux_main",
                  "li a7, 94",
                  "ecall"},
        arch_case{"arm-unknown-linux-gnueabihf",
                  "ldr r0, [sp]",
                  "bl __ghoti_linux_main",
                  "#248",
                  "svc #0"},
        arch_case{"thumbv7-unknown-linux-gnueabihf",
                  "ldr r0, [sp]",
                  "bl __ghoti_linux_main",
                  "#248",
                  "svc #0"},
        arch_case{"loongarch64-unknown-linux-gnu",
                  "ld.d $$a0",
                  "bl __ghoti_linux_main",
                  "$$zero, 94",
                  "syscall 0"})};

    CAPTURE(tc.triple);
    constexpr std::string_view source{R"(
        pub const main = fn(): i32 {
            return 1;
        };
    )"};
    auto [ctx, llvm_mod]{lower_for_triple(source, context, tc.triple)};
    auto& start_fn{UNWRAP(llvm_mod->getFunction("_start"))};
    CHECK(start_fn.hasFnAttribute(llvm::Attribute::Naked));

    const auto text{start_asm_text(start_fn)};
    CAPTURE(text);
    CHECK(text.find(tc.argc_load) != std::string::npos);
    CHECK(text.find(tc.call_main) != std::string::npos);
    CHECK(text.find(tc.exit_nr) != std::string::npos);
    CHECK(text.find(tc.syscall_insn) != std::string::npos);

    // Run the whole `_start` InlineAsm -> MC -> object -> link path so a bad
    // mnemonic for this arch fails here rather than silently at `build-exe` time.
    auto [exe_ctx, exe_idx]{helpers::resolve_for_target(source, tc.triple)};
    codegen::target_options exe_target{.triple_str = std::string{tc.triple}};
    tempfile                exe_file{"test_start_exe"};
    REQUIRE(helpers::emit_executable(*exe_ctx, context, exe_file, exe_target));
    CHECK(path_utils::exists(exe_file));
    CHECK(bin_utils::check_elf_header(exe_file));
}

TEST_CASE("A Linux arch with no freestanding _start is rejected with a diagnostic") {
    codegen::llvm_scope scope;
    llvm::LLVMContext   context;

    auto [ctx, idx]{helpers::resolve_for_target(R"(
        pub const main = fn(): i32 {
            return 0;
        };
    )",
                                                "powerpc64le-unknown-linux-gnu")};
    REQUIRE(ctx->analyzer.validate_main_entry(ctx->root_mod));

    // powerpc64le's target is built in, but std/start.gh has no `_start` for it
    CHECK(lower_error_for_triple(*ctx, context, "powerpc64le-unknown-linux-gnu") ==
          codegen::error::UNSUPPORTED_TARGET);
    CHECK_FALSE(codegen::can_emit_freestanding_entry(
        codegen::resolve_target_triple("powerpc64le-unknown-linux-gnu")));
    CHECK(codegen::can_emit_freestanding_entry(
        codegen::resolve_target_triple("riscv64-unknown-linux-gnu")));
}

TEST_CASE("Non-Linux targets keep the crt-provided entry and get no _start") {
    codegen::llvm_scope        scope;
    llvm::LLVMContext          context;
    constexpr std::string_view source{R"(
        pub const main = fn(): i32 {
            return 0;
        };
    )"};

    SECTION("Windows MinGW") {
        auto [ctx, llvm_mod]{lower_for_triple(source, context, "x86_64-w64-windows-gnu")};
        CHECK_FALSE(llvm_mod->getFunction("_start"));
        CHECK(llvm_mod->getFunction("main"));
        // MinGW's codegen calls `__main` from `main`
        CHECK(llvm_mod->getFunction("__main"));
    }

    SECTION("macOS") {
        auto [ctx, llvm_mod]{lower_for_triple(source, context, "x86_64-apple-macosx")};
        CHECK_FALSE(llvm_mod->getFunction("_start"));
        CHECK(llvm_mod->getFunction("main"));
    }
}

TEST_CASE("main gets every argument, however many and however quoted") {
    codegen::llvm_scope scope;
    // 1 for each argument, plus 100 if the last two arrive intact
    auto [ctx, idx]{helpers::type_check_and_verify(R"(
        const eq = fn(a: [:0]u8, b: []u8): bool {
            if (a.len != b.len) { return false; }
            for (0..a.len) |i| {
                if (a[i] != b[i]) { return false; }
            }
            return true;
        };
        pub const main = fn(args: [][:0]u8): i32 {
            let n = args.len;
            let intact = eq(args[n - 2], "two words") and eq(args[n - 1], "quote\"d");
            return @intCast(i32, n) + if (intact) 100 else 0;
        };
    )",
                                                   {},
                                                   false)};

    llvm::LLVMContext context;
    const auto extension{codegen::get_default_output_extension(codegen::output_type::EXECUTABLE)};
    const tempfile exe{
        std::in_place,
        fmt::format("{}{}", tempfile::make_temp_path("test_many_args").string(), extension)};
    REQUIRE(helpers::emit_executable(*ctx, context, exe.path));

    // More than the 16 (Windows) or 128 (POSIX) arguments the entry once had room for
    std::vector<std::string> argv{exe.path.string()};
    for (usize i{0}; i < 140; ++i) { argv.emplace_back(fmt::format("arg{}", i)); }
    argv.emplace_back("two words");
    argv.emplace_back("quote\"d");
    CHECK(helpers::portable_exit_code(UNWRAP(spawn_child(mock_argv{argv}))) == 143 + 100);
}

TEST_CASE("@export as 'main' picks the entry point") {
    codegen::llvm_scope scope;
    llvm::LLVMContext   context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const my_main = fn(): i32 {
            return 42;
        };
        @export(my_main, .{ .name = "main" });
    )")};
    CHECK(ctx->analyzer.get_ctx().user_main_name == "my_main");
    REQUIRE(ctx->analyzer.validate_main_entry(ctx->root_mod));
    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir_executable(*ctx, context))};

    std::string              err_str;
    llvm::raw_string_ostream os{err_str};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod, &os));

    auto& ghoti_main_fn{UNWRAP(llvm_mod->getFunction("_ghoti_main"))};
    CHECK(ghoti_main_fn.getReturnType()->isIntegerTy(32));
    CHECK(ghoti_main_fn.arg_size() == 0);
    CHECK(llvm_mod->getFunction("main"));
}

TEST_CASE("std/start.gh analyzes on its own, as an editor opens it") {
    std::ostringstream  errors;
    mod::file_loader    loader;
    mod::module_manager manager{loader};
    sema::analyzer      analyzer{manager, errors, false};
    const auto          path{UNWRAP(mod::find_start())};
    CHECK(analyzer.analyze(path));
    const auto module{UNWRAP(manager.try_get_file_module(path))};
    CHECK(module->is_start);
    CHECK_FALSE(module->is_poisoned());
    CHECK(errors.str().empty());
}

TEST_CASE("The entry code's @compileError is reported at the program's main") {
    const tempdir dir{"entry_errors"};
    dir.write("start.gh", R"(const run = fn(): i32 {
    if comptime (@typeInfo(@TypeOf(@entryMain())).function.params.len == 0) {
        @compileError("main must take its arguments");
    }
    return 0;
};
)");
    dir.write("main.gh", "\n\npub const main = fn(): void {};\n");
    set_env("GHOTI_START", (dir.path / "start.gh").string());
    const auto restore{gsl::finally([] { set_env("GHOTI_START", ""); })};

    std::ostringstream  errors;
    mod::file_loader    loader;
    mod::module_manager manager{loader};
    sema::analyzer      analyzer{manager, errors, false};
    analyzer.set_entry_point(true);
    CHECK(analyzer.analyze(dir.path / "main.gh"));

    const auto  root{UNWRAP(manager.try_get_file_module(dir.path / "main.gh"))};
    const auto& diags{UNWRAP(root->diagnostics.as_opt<sema::diagnostics>())};
    REQUIRE(diags.size() == 1);
    const auto& diag{diags[0]};
    CHECK(diag.get_error() == sema::error::COMPILE_ERROR_REACHED);
    CHECK(UNWRAP(diag.to_formattable().location).line == 2);
    const auto message{UNWRAP(diag.get_message())};
    CHECK(message.starts_with("main must take its arguments (required by "));
    CHECK(message.ends_with("start.gh:3:9)"));

    const auto start{UNWRAP(manager.try_get_file_module(dir.path / "start.gh"))};
    CHECK_FALSE(start->is_poisoned());
    CHECK(errors.str().find("main.gh") != std::string::npos);
}

} // namespace ghoti::tests
