#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>

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

// Runs once lib/compiler_rt defines every soft-float routine the program calls; until then it's
// skipped, naming what's missing
auto run_with_builtins(std::string_view source) -> void {
    if (const auto missing{helpers::missing_builtins(source)}; !missing.empty()) {
        SKIP(fmt::format("needs compiler_rt: {}", fmt::join(missing, ", ")));
    }
    CHECK(helpers::compile_and_run(source) == 0);
}

TEST_CASE("f128 arithmetic, comparison, and conversion at runtime") {
    run_with_builtins(R"(
        var a: f128 = 1.5f128;
        var b: f128 = 2.25f128;
        var i: i64 = -7;

        pub const main := fn(): i32 {
            if (a + b != 3.75f128) { return 1; }
            if (b - a != 0.75f128) { return 2; }
            if (a * b != 3.375f128) { return 3; }
            if (b / a != 1.5f128) { return 4; }
            if (!(a < b) or a >= b) { return 5; }
            const widened: f128 = @as(f64, 0.5);
            if (widened != 0.5f128) { return 6; }
            if (@floatFromInt(f128, i) != -7.0f128) { return 7; }
            if (@intFromFloat(i64, b * 4.0f128) != 9) { return 8; }
            return 0;
        };
    )");
}

TEST_CASE("f16 arithmetic and conversion at runtime") {
    run_with_builtins(R"(
        var h: f16 = 1.5f16;
        var k: f16 = 0.25f16;

        pub const main := fn(): i32 {
            if (h + k != 1.75f16) { return 1; }
            if (h * k != 0.375f16) { return 2; }
            const wide: f32 = h;
            if (wide != 1.5) { return 3; }
            return 0;
        };
    )");
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
            return @intFromFloat(i32, @as(f64, c)) - 6; // 0
        };
    )") == 0);
}

// Windows starts x87 at 53-bit precision; the entry wrapper switches it to the full 64 bits
TEST_CASE("f80 keeps its 64-bit significand at runtime") {
    CHECK(helpers::compile_and_run(R"(
        var big: usize = 0x3fffffffffffffff;
        var nearly_one: u80 = 0x3ffeffffffffffffffff;
        pub const main := fn(): i32 {
            const f := @floatFromInt(f80, big);
            if (@bitCast(u80, f) != 0x403cfffffffffffffffc) { return 1; }
            if (@intFromFloat(usize, @bitCast(f80, nearly_one)) != 0) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("f80 encodings x87 rejects fold to NaN like they run") {
    CHECK(helpers::compile_and_run(R"(
        // An unnormal: nonzero exponent with the explicit integer bit clear
        const folded: f80 = @bitCast(f80, @as(u80, 0x40003000000000000000)) + 1.0f80;
        var unnormal: u80 = 0x40003000000000000000;
        pub const main := fn(): i32 {
            if (folded == folded) { return 1; }
            const runtime := @bitCast(f80, unnormal) + 1.0f80;
            if (runtime == runtime) { return 2; }
            return 0;
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
