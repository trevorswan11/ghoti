#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Verifier.h>

#include "compiler/codegen/llvm_lowering.hh"
#include "compiler/gir/emitter.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("LLVM lowering full GIR") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const max := fn(a: i32, b: i32): i32 {
            if (a > b) {
                return a;
            } else {
                return b;
            };
        };

        pub const test_call := fn(): i32 {
            return max(10, 20);
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    codegen::llvm_lowering lowering{context, "full_test_module"};
    auto                   llvm_mod{lowering.lower(gir_mod)};
    REQUIRE(llvm_mod);

    bool broken{llvm::verifyModule(*llvm_mod)};
    CHECK_FALSE(broken);

    auto* lowered_max{llvm_mod->getFunction("max")};
    REQUIRE(lowered_max != nullptr);

    auto* lowered_caller{llvm_mod->getFunction("test_call")};
    REQUIRE(lowered_caller != nullptr);
}

TEST_CASE("LLVM lowering globals and initializers from source") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const GLOBAL_CONST: i32 = 100;
        var global_var: i64 = 500;
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    codegen::llvm_lowering lowering{context, "globals_module"};
    auto                   llvm_mod{lowering.lower(gir_mod)};
    REQUIRE(llvm_mod);

    CHECK_FALSE(llvm::verifyModule(*llvm_mod));
    CHECK(UNWRAP(llvm_mod->getGlobalVariable("GLOBAL_CONST", true)).isConstant());
    CHECK_FALSE(UNWRAP(llvm_mod->getGlobalVariable("global_var", true)).isConstant());
}

TEST_CASE("LLVM lowering loops and mutation") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const sum_to := fn(n: i32): i32 {
            var sum: i32 = 0;
            var i: i32 = 1;
            while (i <= n) {
                sum = sum + i;
                i = i + 1;
            };
            return sum;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    codegen::llvm_lowering lowering{context, "loop_module"};
    auto                   llvm_mod{lowering.lower(gir_mod)};
    REQUIRE(llvm_mod);

    CHECK_FALSE(llvm::verifyModule(*llvm_mod));
    CHECK(llvm_mod->getFunction("sum_to"));
}

} // namespace ghoti::tests
