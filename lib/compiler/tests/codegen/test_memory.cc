#include <string>

#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include "compiler/codegen/llvm_lowering.hh"
#include "compiler/gir/emitter.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("LLVM lowering alloca, store, load, addressOf, deref from source") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const test_pointers = fn(): i32 {
            let mut a: i32 = 42;
            let mut ptr: ^i32 = ^a;
            return *ptr;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    codegen::llvm_lowering lowering{context, "test_memory"};
    auto                   llvm_mod{lowering.lower(gir_mod)};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));
    CHECK(llvm_mod->getFunction("test_pointers"));
}

TEST_CASE("LLVM lowering struct and array indexing") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const get_elem = fn(): i32 {
            let mut arr: [5uz]i32 = [5uz]i32{10, 20, 30, 40, 50};
            return arr[2uz];
        };

        const Point = struct {
            x: i32,
            y: i32,
        };

        pub const get_point_y = fn(): i32 {
            let mut pt = Point{ .x = 1, .y = 42 };
            return pt.y;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    codegen::llvm_lowering lowering{context, "test_gep"};
    auto                   llvm_mod{lowering.lower(gir_mod)};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));
    CHECK(llvm_mod->getFunction("get_elem"));
    CHECK(llvm_mod->getFunction("get_point_y"));
}

TEST_CASE("LLVM lowering volatile load and store") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const test_volatile = fn(): i32 {
            let mut v: volatile i32 = 42;
            v = v + 1;
            return v;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    codegen::llvm_lowering lowering{context, "test_volatile"};
    auto                   llvm_mod{lowering.lower(gir_mod)};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));
    const auto ir_str{helpers::ir_text(*llvm_mod)};
    CHECK(ir_str.contains("store volatile"));
    CHECK(ir_str.contains("load volatile"));
}

TEST_CASE("`@sliceFromPtr` widens an untyped length to the full usize field") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Pair = struct { a: u64, b: u32 };

        pub const as_bytes = fn(p: ^mut Pair): []mut u8 {
            return @sliceFromPtr(@ptrCast(^mut u8, p), @sizeOf(Pair));
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    codegen::llvm_lowering lowering{context, "test_slice_from_ptr"};
    auto                   llvm_mod{lowering.lower(gir_mod)};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));
    // A 32-bit store into the 64-bit length field leaves its upper half uninitialized
    const auto ir_str{helpers::ir_text(*llvm_mod)};
    CHECK(ir_str.contains("store i64 16"));
    CHECK_FALSE(ir_str.contains("store i32 16"));
}

} // namespace ghoti::tests
