#include <string>
#include <string_view>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <llvm/TargetParser/Triple.h>

#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/runtime_libcalls.hh"
#include "compiler/codegen/target.hh"
#include "compiler/sema/analyzer.hh"
#include "compiler/sema/error.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] auto
emit_for(std::string_view source, std::string_view triple, codegen::opt_level level, bool assembly)
    -> std::string {
    auto [ctx, idx]{helpers::resolve_for_target(source, triple)};
    helpers::check_errors<sema::diagnostics>(ctx->root_mod);
    auto gir_mod{ctx->analyzer.emit_gir(ctx->root_mod, false)};
    REQUIRE_FALSE(ctx->root_mod.is_poisoned());
    const codegen::target_options    target_opts{.triple_str = std::string{triple}, .level = level};
    const codegen::optimizer_options opt_opts{.level = level};
    return UNWRAP(assembly ? ctx->analyzer.emit_asm_text(
                                 gir_mod, target_opts, opt_opts, sema::build_artifact::LIBRARY)
                           : ctx->analyzer.emit_llvm_ir_text(
                                 gir_mod, target_opts, opt_opts, sema::build_artifact::LIBRARY));
}

constexpr std::string_view exact_math{R"(
    export const quad = fn(a: f128, b: f128): f128 {
        return @floor(a) + @ceil(b) + @sqrt(a) + a % b;
    };
    export const extended = fn(a: f80, b: f80): f80 {
        return @floor(a) + @ceil(b) + @sqrt(a) + a % b;
    };
)"};

} // namespace

TEST_CASE("f128 floor, ceil, sqrt and % call the f128 routines on every target") {
    // `floorl` and friends are `long double`'s, which is f128 on only some of these
    for (const std::string_view triple : {"x86_64-unknown-linux-gnu",
                                          "x86_64-pc-windows-msvc",
                                          "x86_64-apple-macos",
                                          "aarch64-apple-macos",
                                          "aarch64-unknown-linux-gnu",
                                          "armv7-unknown-linux-gnueabihf",
                                          "riscv64-unknown-linux-gnu",
                                          "wasm32-unknown-unknown"}) {
        INFO(triple);
        const auto asm_text{emit_for(exact_math, triple, codegen::opt_level::O2, true)};
        for (const std::string_view name : {"floorf128", "ceilf128", "sqrtf128", "fmodf128"}) {
            INFO(name);
            CHECK(asm_text.contains(name));
        }
        CHECK_FALSE(asm_text.contains("sqrtl"));
    }
}

TEST_CASE("f80 floor, ceil and % use their own names where long double is double") {
    const auto msvc{emit_for(exact_math, "x86_64-pc-windows-msvc", codegen::opt_level::O0, false)};
    CHECK(msvc.contains("call x86_fp80 @floorf64x("));
    CHECK(msvc.contains("call x86_fp80 @ceilf64x("));
    CHECK(msvc.contains("call x86_fp80 @fmodf64x("));
    // x87's `fsqrt` needs no routine
    CHECK(msvc.contains("@llvm.sqrt.f80"));

    // Where `long double` is x87, LLVM's own `floorl`/`fmodl` are already right
    const auto linux_out{
        emit_for(exact_math, "x86_64-unknown-linux-gnu", codegen::opt_level::O0, false)};
    CHECK(linux_out.contains("@llvm.floor.f80"));
    CHECK(linux_out.contains("frem x86_fp80"));

    const auto msvc_triple{codegen::resolve_target_triple("x86_64-pc-windows-msvc")};
    for (const std::string_view name : {"floorf128", "fmodf128", "sinf128", "fmodf64x"}) {
        INFO(name);
        CHECK(codegen::is_compiler_rt_symbol(msvc_triple, name));
    }
    CHECK_FALSE(codegen::is_compiler_rt_symbol(msvc_triple, "fmodf129"));
}

TEST_CASE("128-bit atomics are native on x86-64") {
    constexpr std::string_view source{R"(
        export const bump = fn(p: ^mut u128): u128 {
            return @atomicRmw(u128, p, .add, 1, .seq_cst) + @atomicLoad(u128, p, .acquire);
        };
    )"};
    for (const std::string_view triple :
         {"x86_64-unknown-linux-gnu", "x86_64-pc-windows-msvc", "x86_64-apple-macos"}) {
        INFO(triple);
        const auto asm_text{emit_for(source, triple, codegen::opt_level::O2, true)};
        CHECK(asm_text.contains("cmpxchg16b"));
        CHECK_FALSE(asm_text.contains("__atomic"));
    }
}

TEST_CASE("64-bit atomics on i686 stay instructions where the ABI aligns u64 to 4") {
    constexpr std::string_view source{R"(
        export const bump = fn(p: ^mut u64): u64 {
            @atomicStore(p, 3, .release);
            return @atomicRmw(u64, p, .add, 1, .seq_cst) + @atomicLoad(u64, p, .acquire);
        };
    )"};
    for (const std::string_view triple : {"i686-unknown-linux-gnu", "i686-pc-windows-msvc"}) {
        INFO(triple);
        const auto ir{emit_for(source, triple, codegen::opt_level::O0, false)};
        CHECK(ir.contains("load atomic i64, ptr %"));
        CHECK(ir.contains("acquire, align 8"));
        CHECK(ir.contains("release, align 8"));
        CHECK_FALSE(emit_for(source, triple, codegen::opt_level::O2, true).contains("__atomic"));
    }
}

TEST_CASE("RISC-V Linux defaults to RV64GC with the hard-float ABI") {
    constexpr std::string_view source{R"(
        export const mix = fn(a: f64, b: f64, n: u64, d: u64): f64 {
            return a * b + @floatFromInt(f64, n / d);
        };
        export const bump = fn(p: ^mut u32): u32 {
            return @atomicRmw(u32, p, .add, 1, .seq_cst);
        };
    )"};
    const auto                 linux_out{
        emit_for(source, "riscv64-unknown-linux-gnu", codegen::opt_level::O2, true)};
    CHECK(linux_out.contains("fmul.d"));
    CHECK(linux_out.contains("divu"));
    CHECK(linux_out.contains("amoadd.w"));
    CHECK_FALSE(linux_out.contains("__muldf3"));
    CHECK_FALSE(linux_out.contains("__atomic"));
    // A double argument arrives in `fa0`, as glibc's lp64d expects
    CHECK(linux_out.contains("fa0"));

    // Bare metal keeps the bare `generic` CPU
    const auto elf{emit_for(source, "riscv64-unknown-elf", codegen::opt_level::O2, true)};
    CHECK(elf.contains("__muldf3"));
}

} // namespace ghoti::tests
