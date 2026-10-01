#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

auto run_with_builtins(std::string_view source) -> void {
    if (const auto missing{helpers::missing_builtins(source)}; !missing.empty()) {
        SKIP(fmt::format("needs compiler_rt: {}", fmt::join(missing, ", ")));
    }
    CHECK(helpers::compile_and_run(source) == 0);
}

} // namespace

TEST_CASE("math builtins fold at compile time, correctly rounded") {
    // The expected bits are mpmath's results rounded to nearest
    CHECK(helpers::compile_and_run(R"(
        const e: f64 = @exp(1.0);
        const root: f64 = @sqrt(2.0);
        const sine: f32 = @sin(0.1);
        const ln10: f64 = @log(10.0);
        pub const main := fn(): i32 {
            if (@bitCast(u64, e) != 0x4005bf0a8b145769u64) { return 1; }
            if (@bitCast(u64, root) != 0x3ff6a09e667f3bcdu64) { return 2; }
            if (@bitCast(u32, sine) != 0x3dcc7576u32) { return 3; }
            if (@bitCast(u64, ln10) != 0x40026bb1bbb55516u64) { return 4; }
            if (@log2(1024.0) != 10.0 or @log10(1000.0) != 3.0 or @exp2(-3.0) != 0.125) {
                return 5;
            }
            if (@floor(-2.5) != -3.0 or @ceil(-2.5) != -2.0 or @floor(7.0) != 7.0) { return 6; }
            if (@cos(0.0) != 1.0 or @tan(0.0) != 0.0) { return 7; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("an untyped math result is computed in the type it lands in") {
    // Rounding the f128 result to f32 again would differ from the direct f32 result somewhere;
    // here both paths must give the direct one
    CHECK(helpers::compile_and_run(R"(
        const untyped := @sin(0.1);
        const later: f32 = untyped;
        const direct: f32 = @sin(0.1);
        pub const main := fn(): i32 {
            if (@TypeOf(untyped) != @TypeOf(0.1)) { return 1; }
            if (@bitCast(u32, later) != @bitCast(u32, direct)) { return 2; }
            let passed: f32 = untyped;
            if (@bitCast(u32, passed) != 0x3dcc7576u32) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("a math builtin keeps its operand's float type") {
    helpers::type_check_and_verify(R"(
        pub const f := fn(a: f32, b: f64, c: f16, d: f128): void {
            let x: f32 = @sin(a);
            let y: f64 = @log(b);
            let z: f16 = @sqrt(c);
            let w: f128 = @floor(d);
            _ = x; _ = y; _ = z; _ = w;
        };
    )");
}

TEST_CASE("math builtins reject integers, types, and results past the range") {
    helpers::test_checker_fail(R"(
        pub const f := fn(n: i32): void {
            let x := @sqrt(n);
        };
    )",
                               sema::diagnostic{"'@sqrt' operand must be a float; found 'i32'; "
                                                "convert it with `@floatFromInt`",
                                                sema::error::TYPE_MISMATCH,
                                                std::pair{2UZ, 27UZ}});
    helpers::expect_compile_error("pub const main := fn(): i32 { let x := @sin(f32); return 0; };");
    helpers::expect_compile_error(
        "pub const main := fn(): i32 { let b := @exp(true); return 0; };");

    const auto [ctx, idx]{helpers::expect_compile_error(R"(
        const big: f64 = @exp(1000.0);
    )")};
    const auto& diags{ctx->root_mod.diagnostics.as<sema::diagnostics>()};
    CHECK(std::ranges::any_of(diags, [](const auto& diag) {
        return diag.get_error() == sema::error::LITERAL_OUT_OF_RANGE;
    }));
    // A pole is a value, not an error
    CHECK(helpers::compile_and_run(R"(
        const pole: f64 = @log(0.0);
        pub const main := fn(): i32 { return if (pole < -1.0e308) 0 else 1; };
    )") == 0);
}

TEST_CASE("runtime math calls the target's routines, never LLVM's foldable intrinsics") {
    llvm::LLVMContext context;
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const g := fn(q: f128): f128 { return @exp(q); };
        pub const f := fn(a: f32, b: f64, h: f16): f64 {
            let s := @sin(a);
            let r := @sqrt(b);
            let l := @log10(h);
            return @floatCast(f64, s) + r + @floatCast(f64, l);
        };
    )")};
    auto       llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    const auto ir{helpers::ir_text(*llvm_mod)};
    CHECK(ir.contains("@sinf("));
    CHECK(ir.contains("llvm.sqrt.f64"));
    CHECK(ir.contains("@log10f("));  // f16 goes through the f32 routine
    CHECK(ir.contains("@expf128(")); // Never `expl`, which is `double` or x87 on some platforms
    CHECK_FALSE(ir.contains("llvm.sin"));
    CHECK(ir.contains("nobuiltin"));
}

TEST_CASE("runtime math matches compile-time math") {
    run_with_builtins(R"(
        let mut one: f64 = 1.0;
        let mut tenth: f32 = 0.1f32;
        let mut two: f64 = 2.0;
        pub const main := fn(): i32 {
            if (@exp(one) != @exp(1.0)) { return 1; }
            if (@sin(tenth) != @sin(0.1f32)) { return 2; }
            if (@sqrt(two) != @sqrt(2.0)) { return 3; }
            if (@floor(-two) != -2.0 or @ceil(tenth) != 1.0f32) { return 4; }
            if (@log2(two) != 1.0 or @exp2(two) != 4.0) { return 5; }
            return 0;
        };
    )");
}

} // namespace ghoti::tests
