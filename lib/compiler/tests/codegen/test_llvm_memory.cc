#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Verifier.h>

#include "compiler/codegen/llvm_lowering.hh"
#include "compiler/gir/emitter.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("LLVM lowering alloca, store, load, addressOf, deref from source") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const test_pointers := fn(): i32 {
            var a: i32 = 42;
            var ptr: ^i32 = ^a;
            return *ptr;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    codegen::llvm_lowering lowering{context, "test_memory"};
    auto                   llvm_mod{lowering.lower(gir_mod)};
    REQUIRE(llvm_mod);

    CHECK_FALSE(llvm::verifyModule(*llvm_mod));
    CHECK(llvm_mod->getFunction("test_pointers"));
}

TEST_CASE("LLVM lowering struct and array indexing") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const get_elem := fn(): i32 {
            var arr: [5uz]i32 = [5uz]i32{10, 20, 30, 40, 50};
            return arr[2uz];
        };

        const Point := struct {
            x: i32,
            y: i32,
        };

        pub const get_point_y := fn(): i32 {
            var pt := Point{ .x = 1, .y = 42 };
            return pt.y;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    codegen::llvm_lowering lowering{context, "test_gep"};
    auto                   llvm_mod{lowering.lower(gir_mod)};
    REQUIRE(llvm_mod);

    CHECK_FALSE(llvm::verifyModule(*llvm_mod));
    CHECK(llvm_mod->getFunction("get_elem"));
    CHECK(llvm_mod->getFunction("get_point_y"));
}

} // namespace ghoti::tests
