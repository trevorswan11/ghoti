#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Verifier.h>

#include "compiler/codegen/llvm_lowering.hh"
#include "compiler/gir/emitter.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("LLVM lowering for @returnAddress") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const get_caller_addr := fn(): usize {
            return @returnAddress();
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    codegen::llvm_lowering lowering{context, "test_return_address_module"};
    auto                   llvm_mod{lowering.lower(gir_mod)};
    REQUIRE(llvm_mod);

    bool broken{llvm::verifyModule(*llvm_mod)};
    CHECK_FALSE(broken);

    const auto ir{helpers::ir_text(*llvm_mod)};
    CHECK(ir.contains("llvm.returnaddress"));
    CHECK(ir.contains("ptrtoint"));
}

} // namespace ghoti::tests
