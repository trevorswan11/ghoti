#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include "compiler/sema/analyzer.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("Codegen: valueless extern global lowers as a declaration") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        extern var errno_val: i32;
        pub const main := fn(args: [][:0]u8): void {};
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));

    auto& gv{UNWRAP(llvm_mod->getGlobalVariable("errno_val"))};
    CHECK(gv.isDeclaration());
    CHECK_FALSE(gv.hasInitializer());
    CHECK(gv.getLinkage() == llvm::GlobalValue::ExternalLinkage);
}

TEST_CASE("Codegen: same extern global from root and imported module does not collide") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(
        R"(
            import "libc.gh" as libc;
            extern var errno_val: i32;
            pub const main := fn(args: [][:0]u8): void {};
        )",
        {helpers::mock_file{"libc.gh", "pub extern var errno_val: i32;", "libc"}})};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));

    auto& gv{UNWRAP(llvm_mod->getGlobalVariable("errno_val"))};
    CHECK(gv.isDeclaration());
    CHECK_FALSE(gv.hasInitializer());
}

TEST_CASE("Codegen: global with an initializer stays a definition") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const answer: i32 = 42;
        pub const main := fn(args: [][:0]u8): void {};
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));

    auto& gv{UNWRAP(llvm_mod->getGlobalVariable("answer"))};
    CHECK(gv.hasInitializer());
    CHECK_FALSE(gv.isDeclaration());
}

} // namespace ghoti::tests
