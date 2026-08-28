#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>

#include "compiler/sema/analyzer.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("Codegen: extern link-name override renames the imported symbol") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        extern("c", "__errno_location") const errno_loc: fn(): ^mut i32;
        pub const main := fn(args: [][:0]u8): void {
            const p := errno_loc();
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));

    CHECK(llvm_mod->getFunction("__errno_location"));
    CHECK_FALSE(llvm_mod->getFunction("errno_loc"));
}

TEST_CASE("Codegen: export link-name override renames the exported symbol") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        export("ghoti_add") const add := fn(a: i32, b: i32): i32 { return a + b; };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));

    auto& renamed{UNWRAP(llvm_mod->getFunction("ghoti_add"))};
    CHECK(renamed.getLinkage() == llvm::GlobalValue::ExternalLinkage);
    CHECK_FALSE(llvm_mod->getFunction("add"));
}

TEST_CASE("Codegen: threadlocal extern global gets a TLS model") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        threadlocal extern var tls_errno: i32;
        pub const main := fn(args: [][:0]u8): void {};
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));

    auto& gv{UNWRAP(llvm_mod->getGlobalVariable("tls_errno"))};
    CHECK(gv.isThreadLocal());
    CHECK(gv.isDeclaration());
}

TEST_CASE("Codegen: weak extern function uses extern_weak linkage") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        weak extern const maybe_present: fn(): void;
        pub const main := fn(args: [][:0]u8): void {
            maybe_present();
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));

    auto& fn{UNWRAP(llvm_mod->getFunction("maybe_present"))};
    CHECK(fn.hasExternalWeakLinkage());
}

TEST_CASE("Codegen: weak definition uses weak_any linkage") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub weak const overridable := fn(): i32 { return 1; };
        pub const main := fn(args: [][:0]u8): void {};
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));

    auto& fn{UNWRAP(llvm_mod->getFunction("overridable"))};
    CHECK(fn.hasWeakAnyLinkage());
}

TEST_CASE("Codegen: naked function carries the naked attribute and no synthesized return") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const trap_stub := naked fn(): void {
            asm {
                template: "",
                options: (volatile, noreturn),
            };
        };
        pub const main := fn(args: [][:0]u8): void {};
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));

    auto& fn{UNWRAP(llvm_mod->getFunction("trap_stub"))};
    CHECK(fn.hasFnAttribute(llvm::Attribute::Naked));
    CHECK(fn.hasFnAttribute(llvm::Attribute::NoInline));

    bool has_ret{false};
    for (auto& bb : fn) {
        for (auto& inst : bb) {
            if (llvm::isa<llvm::ReturnInst>(&inst)) { has_ret = true; }
        }
    }
    CHECK_FALSE(has_ret);
}

} // namespace ghoti::tests
