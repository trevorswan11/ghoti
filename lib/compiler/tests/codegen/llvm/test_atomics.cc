#include <string>

#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "compiler/codegen/llvm_scope.hh"
#include "compiler/codegen/opt_level.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("@atomicLoad lowers to an atomic load with the requested ordering") {
    codegen::llvm_scope scope;
    llvm::LLVMContext   context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const f := fn(p: ^mut i32): i32 {
            return @atomicLoad(i32, p, builtin::MemoryOrder.seq_cst);
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context, {.level = codegen::opt_level::O0}))};
    const auto ir{helpers::ir_text(*llvm_mod)};
    CHECK(ir.find("load atomic i32") != std::string::npos);
    CHECK(ir.find("seq_cst") != std::string::npos);
}

TEST_CASE("@atomicStore lowers to an atomic store with the requested ordering") {
    codegen::llvm_scope scope;
    llvm::LLVMContext   context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const f := fn(p: ^mut i32): void {
            @atomicStore(p, 1, builtin::MemoryOrder.release);
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context, {.level = codegen::opt_level::O0}))};
    const auto ir{helpers::ir_text(*llvm_mod)};
    CHECK(ir.find("store atomic i32") != std::string::npos);
    CHECK(ir.find("release") != std::string::npos);
}

TEST_CASE("@atomicRmw lowers to an atomicrmw instruction using the requested op") {
    codegen::llvm_scope scope;
    llvm::LLVMContext   context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const f := fn(p: ^mut i32): i32 {
            return @atomicRmw(i32, p, builtin::AtomicRmwOp.add, 1, builtin::MemoryOrder.seq_cst);
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context, {.level = codegen::opt_level::O0}))};
    const auto ir{helpers::ir_text(*llvm_mod)};
    CHECK(ir.find("atomicrmw add") != std::string::npos);
}

TEST_CASE("@cmpxchgWeak lowers to a weak cmpxchg instruction") {
    codegen::llvm_scope scope;
    llvm::LLVMContext   context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const f := fn(p: ^mut i32, out: ^mut i32): bool {
            return @cmpxchgWeak(i32, p, 0, 1, builtin::MemoryOrder.seq_cst,
                                builtin::MemoryOrder.relaxed, out);
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context, {.level = codegen::opt_level::O0}))};
    const auto ir{helpers::ir_text(*llvm_mod)};
    CHECK(ir.find("cmpxchg weak") != std::string::npos);
}

TEST_CASE("@cmpxchgStrong lowers to a (non-weak) cmpxchg instruction") {
    codegen::llvm_scope scope;
    llvm::LLVMContext   context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const f := fn(p: ^mut i32, out: ^mut i32): bool {
            return @cmpxchgStrong(i32, p, 0, 1, builtin::MemoryOrder.seq_cst,
                                  builtin::MemoryOrder.relaxed, out);
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context, {.level = codegen::opt_level::O0}))};
    const auto ir{helpers::ir_text(*llvm_mod)};
    CHECK(ir.find("cmpxchg ") != std::string::npos);
    CHECK(ir.find("cmpxchg weak") == std::string::npos);
}

TEST_CASE("@fence lowers to a standalone fence instruction") {
    codegen::llvm_scope scope;
    llvm::LLVMContext   context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const f := fn(): void {
            @fence(builtin::MemoryOrder.acq_rel);
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context, {.level = codegen::opt_level::O0}))};
    const auto ir{helpers::ir_text(*llvm_mod)};
    CHECK(ir.find("fence acq_rel") != std::string::npos);
}

} // namespace ghoti::tests
