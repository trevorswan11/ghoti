#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include "compiler/sema/error.hh"
#include "ghoti/config.h"
#include "helpers/codegen.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

// f16/f128 arithmetic needs compiler-rt soft-float helpers that the test link does not
// provide, so those are exercised at the type/IR level rather than by running.
TEST_CASE("f16 and f128 type-check, widen, and lower to the expected LLVM types") {
    helpers::resolve_and_check(R"(
        pub const main := fn(): i32 {
            var h: f16 = 1.5f16;
            h = h + 0.5f16;
            const to_f32: f32 = h;                // f16 -> f32
            const to_f128: f128 = to_f32;         // f32 -> f128
            var q: f128 = 2.0f128;
            q = q * 3.0f128;
            _ = to_f128;
            return 0;
        };
    )");

    llvm::LLVMContext context;
    auto [ir_ctx, ir_idx]{helpers::resolve_and_check(R"(
        pub const use_ext := fn(a: f16, b: f128): f128 {
            const w: f128 = a;
            return w + b;
        };
    )")};
    auto       llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ir_ctx, context))};
    const auto ir{helpers::ir_text(*llvm_mod)};
    CHECK(ir.find("half") != std::string::npos);
    CHECK(ir.find("fp128") != std::string::npos);
}

TEST_CASE("@sizeOf of the extended float types") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            if (@sizeOf(f16) != 2) { return 1; }
            if (@sizeOf(f32) != 4 or @sizeOf(f64) != 8) { return 2; }
            if (@sizeOf(f128) != 16) { return 3; }
            return 0;
        };
    )") == 0);
}

#if GHOTI_ASM_HOST_X86_64
TEST_CASE("f80 is accepted on the x86-64 host") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: f80 = 10.0f80;
            var b: f80 = 4.0f80;
            const c: f80 = a - b;                 // 6.0
            return @as(i32, @as(f64, c)) - 6;     // 0
        };
    )") == 0);
}
#endif

TEST_CASE("f80 is rejected on non-x86 targets") {
    auto [ctx,
          idx]{helpers::resolve_for_target("var x: f80 = undefined;", "aarch64-unknown-linux-gnu")};
    helpers::check_errors_against<sema::diagnostics>(
        ctx->root_mod,
        sema::diagnostic{"the 'f80' type is only available on x86 and x86_64 targets",
                         sema::error::UNSUPPORTED_TARGET,
                         std::pair{0UZ, 7UZ}});
}

} // namespace ghoti::tests
