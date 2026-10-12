#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fmt/format.h>
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

#include "catch2/catch_message.hpp"
#include "compiler/codegen/error.hh"
#include "compiler/codegen/llvm_scope.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/bin_utils.hh"
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

    SECTION("Main with non-void and non-i32 return returns error") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            pub const main = fn(args: [][:0]u8): bool {
                return true;
            };
        )")};
        CHECK_FALSE(ctx->analyzer.validate_main_entry(ctx->root_mod));
    }

    SECTION("Main with i32 return is valid") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            pub const main = fn(args: [][:0]u8): i32 {
                return 0;
            };
        )")};
        CHECK(ctx->analyzer.validate_main_entry(ctx->root_mod));
    }

    SECTION("Main with incorrect param type returns error") {
        constexpr auto input = R"(
            pub const main = fn(args: []u8): void {
                return;
            };
        )";
        auto [ctx, idx]{helpers::resolve_and_check(input)};
        CHECK_FALSE(ctx->analyzer.validate_main_entry(ctx->root_mod));
    }
}

TEST_CASE("An executable enters through lib/start, which calls main") {
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
    CHECK(ghoti_main_fn.arg_size() == 1);

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

    // powerpc64le's target is built in, but lib/start has no `_start` for it
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

} // namespace ghoti::tests
