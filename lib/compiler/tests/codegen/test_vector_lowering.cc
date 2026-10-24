#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] auto lowered(std::string_view source) -> std::string {
    llvm::LLVMContext context;
    auto [ctx, idx]{helpers::resolve_and_check(source)};
    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod, &llvm::errs()));
    return helpers::ir_text(*llvm_mod);
}

} // namespace

TEST_CASE("A masked load or store that picks every lane is a plain unaligned one") {
    const auto ir{lowered(R"(
        pub const copy4 = fn(src: ^i32, dst: ^mut i32): void {
            let v = @maskedLoad(src, @as(@Vector(4, bool), @splat(true)), undefined);
            @maskedStore(dst, v, @as(@Vector(4, bool), @splat(true)));
        };
        pub const some = fn(src: ^i32, m: @Vector(4, bool)): @Vector(4, i32) {
            return @maskedLoad(src, m, @as(@Vector(4, i32), @splat(0)));
        };
    )")};
    CHECK(ir.contains("load <4 x i32>, ptr"));
    CHECK(ir.contains("store <4 x i32>"));
    CHECK(ir.contains(", align 4"));
    // Only the runtime mask needs the intrinsic
    CHECK(ir.contains("call <4 x i32> @llvm.masked.load.v4i32.p0"));
    CHECK_FALSE(ir.contains("@llvm.masked.store"));
}

TEST_CASE("Lane builtins lower to LLVM's vector instructions") {
    const auto ir{lowered(R"(
        pub const f = fn(a: @Vector(4, i32), b: @Vector(4, i32)): i32 {
            let s = @shuffle(a, b, .{ 0, -1, 3, -4 });
            let m = @select(a > b, a, b);
            return @reduce(.add, s) + @reduce(.max, m);
        };
    )")};
    CHECK(ir.contains("shufflevector <4 x i32>"));
    CHECK(ir.contains("<4 x i32> <i32 0, i32 4, i32 3, i32 7>"));
    CHECK(ir.contains("select <4 x i1>"));
    CHECK(ir.contains("@llvm.vector.reduce.add.v4i32"));
    CHECK(ir.contains("@llvm.vector.reduce.smax.v4i32"));
}

} // namespace ghoti::tests
