#include <string>

#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("codegen: inline asm lowers to an LLVM InlineAsm call") {
    llvm::LLVMContext context;
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const sys_write := fn(fd: i64, buf: ^u8, len: usize): i64 {
            var ret: i64 = 0i64;
            asm {
                template: "syscall",
                outputs: ("={rax}" = ret),
                inputs: ("{rax}" = 1i64, "{rdi}" = fd, "{rsi}" = buf, "{rdx}" = len),
                clobbers: ("rcx", "r11", "memory"),
                options: (volatile),
            };
            return ret;
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod, &llvm::errs()));

    const auto ir{helpers::ir_text(*llvm_mod)};
    CHECK(ir.contains("call i64 asm sideeffect \"syscall\""));
    CHECK(ir.contains("={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}"));
}

TEST_CASE("codegen: noreturn inline asm marks the call and emits unreachable") {
    llvm::LLVMContext context;
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const halt := fn(code: i64): void {
            asm {
                template: "syscall",
                inputs: ("{rax}" = 60i64, "{rdi}" = code),
                options: (volatile, noreturn),
            };
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod, &llvm::errs()));

    const auto ir{helpers::ir_text(*llvm_mod)};
    CHECK(ir.contains("asm sideeffect \"syscall\""));
    CHECK(ir.contains("unreachable"));
}

TEST_CASE("codegen: rdtsc via result slot returns the scalar") {
    llvm::LLVMContext context;
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const timestamp := fn(): u32 {
            return asm u32 {
                template: "rdtsc",
                outputs: ("={eax}" = _),
                options: (volatile),
            };
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod, &llvm::errs()));

    const auto ir{helpers::ir_text(*llvm_mod)};
    CHECK(ir.contains("call i32 asm sideeffect \"rdtsc\", \"={eax}\""));
}

} // namespace ghoti::tests
