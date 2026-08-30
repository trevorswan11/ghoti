#include <string>

#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include "compiler/codegen/llvm_scope.hh"
#include "compiler/codegen/opt_level.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("runtime safety on: arithmetic and indexing emit guards") {
    codegen::llvm_scope scope;
    llvm::LLVMContext   context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const f := fn(a: i32, b: i32, xs: []i32, i: i32): i32 {
            return a + b + xs[i];
        };
    )")};
    CHECK(ctx->analyzer.get_ctx().runtime_safety);

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context, {.level = codegen::opt_level::O0}))};
    const auto ir{helpers::ir_text(*llvm_mod)};
    CHECK(ir.find("sadd.with.overflow") != std::string::npos);
    CHECK(ir.find("panic_handler") != std::string::npos);
}

TEST_CASE("runtime safety off: no arithmetic or bounds guards are emitted") {
    codegen::llvm_scope scope;
    llvm::LLVMContext   context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const f := fn(a: i32, b: i32, xs: []i32, i: i32): i32 {
            return a + b + xs[i];
        };
    )")};
    ctx->analyzer.get_ctx().runtime_safety = false;

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context, {.level = codegen::opt_level::O0}))};
    const auto ir{helpers::ir_text(*llvm_mod)};
    CHECK(ir.find("with.overflow") == std::string::npos);
    CHECK(ir.find("panic_handler") == std::string::npos);
    CHECK(ir.find("safety.fail") == std::string::npos);
}

} // namespace ghoti::tests
