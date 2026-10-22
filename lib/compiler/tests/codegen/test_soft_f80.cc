#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <llvm/TargetParser/Triple.h>

#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/runtime_libcalls.hh"
#include "compiler/codegen/soft_f80.hh"
#include "compiler/codegen/target.hh"
#include "compiler/sema/analyzer.hh"
#include "compiler/sema/error.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"
#include "support/float128.hh"
#include "support/float_math.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view operations{R"(
export const arith = fn(a: f80, b: f80): f80 {
    return (a + b) * (a - b) / b + a % b + -a;
};

export const compare = fn(a: f80, b: f80): u8 {
    let mut r: u8 = 0;
    if (a == b) { r |= 1; }
    if (a != b) { r |= 2; }
    if (a < b) { r |= 4; }
    if (a <= b) { r |= 8; }
    if (a > b) { r |= 16; }
    if (a >= b) { r |= 32; }
    return r;
};

export const widen = fn(h: f16, s: f32, d: f64): f80 {
    let x: f80 = h;
    let y: f80 = s;
    let z: f80 = d;
    return x + y + z;
};

export const narrow = fn(x: f80): f64 {
    return @floatCast(f64, x) + @floatCast(f32, x) + @floatCast(f16, x);
};

export const quad = fn(x: f80): f80 {
    let q: f128 = x;
    return @floatCast(f80, q);
};

export const from_ints = fn(a: i8, b: u32, c: i64, d: u128, e: i200): f80 {
    return @floatFromInt(f80, a) + @floatFromInt(f80, b) + @floatFromInt(f80, c) +
        @floatFromInt(f80, d) + @floatFromInt(f80, e);
};

export const to_ints = fn(x: f80): i64 {
    let a = @intFromFloat(i16, x);
    let b = @intFromFloat(u64, x);
    let c = @intFromFloat(i128, x);
    let d = @intFromFloat(u200, x);
    return a + @as(i64, @bitCast(b)) + @as(i64, @truncate(c)) + @as(i64, @truncate(d));
};

export const math = fn(x: f80): f80 {
    return @sqrt(x) + @floor(x) + @ceil(x) + @sin(x) + @log2(x) + @abs(x) +
        @mulAdd(f80, x, x, x);
};

export let mut third: f80 = 1.0 / 3.0;
)"};

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

} // namespace

TEST_CASE("f80 is accepted on every target") {
    for (const std::string_view triple :
         {"aarch64-unknown-linux-gnu", "riscv64-unknown-linux-gnu", "wasm32-unknown-unknown"}) {
        INFO(triple);
        auto [ctx, idx]{helpers::type_check_for_target(operations, triple)};
        helpers::check_errors<sema::diagnostics>(ctx->root_mod);
    }
}

TEST_CASE("f80 operations off x86 lower to compiler_rt calls") {
    const auto ir{emit_for(operations, "aarch64-unknown-linux-gnu", codegen::opt_level::O0, false)};
    CHECK_FALSE(ir.contains("x86_fp80"));
    for (const std::string_view call : {
             "call i80 @__addxf3(i80",
             "call i80 @__subxf3(",
             "call i80 @__mulxf3(",
             "call i80 @__divxf3(",
             "call i80 @__fmodx(",
             "call i32 @__eqxf2(",
             "call i32 @__nexf2(",
             "call i32 @__ltxf2(",
             "call i32 @__lexf2(",
             "call i32 @__gtxf2(",
             "call i32 @__gexf2(",
             "call i80 @__extendhfxf2(half",
             "call i80 @__extendsfxf2(float",
             "call i80 @__extenddfxf2(double",
             "call fp128 @__extendxftf2(",
             "call double @__truncxfdf2(",
             "call float @__truncxfsf2(",
             "call half @__truncxfhf2(",
             "call i80 @__trunctfxf2(fp128",
             "call i80 @__floatsixf(i32",
             "call i80 @__floatunsixf(i32",
             "call i80 @__floatdixf(i64",
             "call i80 @__floatuntixf(i128",
             "call i80 @__floateixf(ptr",
             "call i32 @__fixxfsi(",
             "call i64 @__fixunsxfdi(",
             "call i128 @__fixxfti(",
             "call void @__fixunsxfei(ptr",
             "call i80 @__sqrtx(",
             "call i80 @__floorx(",
             "call i80 @__ceilx(",
             "call i80 @__sinx(",
             "call i80 @__log2x(",
             "call i80 @__fmax(",
         }) {
        INFO(call);
        CHECK(ir.contains(call));
    }
    // A sign-bit operation needs no routine, and a constant keeps its bits
    CHECK_FALSE(ir.contains("@llvm.fabs"));
    CHECK(ir.contains("@third = global i80 302188412500818638056107, align 16"));
}

TEST_CASE("f80 compiles to native code on targets without x87") {
    for (const std::string_view triple : {"aarch64-unknown-linux-gnu",
                                          "riscv64-unknown-linux-gnu",
                                          "arm-unknown-linux-gnueabihf",
                                          "wasm32-unknown-unknown"}) {
        for (const auto level : {codegen::opt_level::O0, codegen::opt_level::O2}) {
            INFO(triple << (level == codegen::opt_level::O0 ? " -O0" : " -O2"));
            const auto assembly{emit_for(operations, triple, level, true)};
            CHECK(assembly.contains("__addxf3"));
            CHECK(assembly.contains("__ltxf2"));
        }
    }
}

TEST_CASE("a conversion between any float and an integer wider than 128 bits compiles off x86") {
    constexpr std::string_view source{R"(
        export const from_wide = fn(a: i200, b: u256): f64 {
            return @floatFromInt(f64, a) + @floatFromInt(f32, b) + @floatFromInt(f16, a) +
                @floatCast(f64, @floatFromInt(f128, b));
        };
        export const to_wide = fn(h: f16, s: f32, d: f64, q: f128): i200 {
            return @intFromFloat(i200, h) + @intFromFloat(i200, s) + @intFromFloat(i200, d) +
                @as(i200, @intCast(@intFromFloat(u199, q)));
        };
    )"};

    const auto ir{emit_for(source, "aarch64-unknown-linux-gnu", codegen::opt_level::O0, false)};
    for (const std::string_view call : {
             "call double @__floateidf(ptr",
             "call float @__floatuneisf(ptr",
             "call half @__floateihf(ptr",
             "call fp128 @__floatuneitf(ptr",
             "call void @__fixhfei(ptr",
             "call void @__fixsfei(ptr",
             "call void @__fixdfei(ptr",
             "call void @__fixunstfei(ptr",
         }) {
        INFO(call);
        CHECK(ir.contains(call));
    }

    for (const std::string_view triple : {"aarch64-unknown-linux-gnu",
                                          "riscv32-unknown-linux-gnu",
                                          "arm-unknown-linux-gnueabihf",
                                          "wasm32-unknown-unknown"}) {
        INFO(triple);
        CHECK(emit_for(source, triple, codegen::opt_level::O2, true).contains("__fixdfei"));
    }
    CHECK(codegen::is_compiler_rt_symbol(
        codegen::resolve_target_triple("aarch64-unknown-linux-gnu"), "__floateisf"));
}

TEST_CASE("an aggregate holding an f80 keeps its layout off x86") {
    // 32-bit Arm and RISC-V align an `i80` to 8, but sema laid the `f80` out at 16
    constexpr std::string_view source{R"(
        const S = struct { a: u8, b: f80, c: u8 };
        comptime { @assert(@sizeOf(S) == 48 and @sizeOf([3]f80) == 48); }
        export const middle = fn(s: ^S, i: usize): f80 {
            let pair: [2]S = .{ *s, *s };
            return pair[i].b + @floatFromInt(f80, pair[i].c);
        };
        export const captured = fn(x: f80, tag: u8): f80 {
            let add = fn(y: f80): f80 { return x + y + @floatFromInt(f80, tag); };
            return add(x);
        };
    )"};
    constexpr std::string_view padded{"{ i8, [15 x i8], i80, i8, [15 x i8] }"};
    for (const auto& [triple, layout] :
         {std::pair{"arm-unknown-linux-gnueabihf", padded},
          std::pair{"riscv32-unknown-linux-gnu", padded},
          std::pair{"aarch64-unknown-linux-gnu", std::string_view{"{ i8, i80, i8 }"}}}) {
        INFO(triple);
        const auto ir{emit_for(source, triple, codegen::opt_level::O0, false)};
        CHECK(ir.contains(layout));
        CHECK_FALSE(emit_for(source, triple, codegen::opt_level::O2, true).empty());
    }
}

TEST_CASE("x86 keeps its native f80 instructions") {
    const auto ir{emit_for(operations, "x86_64-unknown-linux-gnu", codegen::opt_level::O0, false)};
    CHECK(ir.contains("fadd x86_fp80"));
    CHECK(ir.contains("fcmp olt x86_fp80"));
    CHECK_FALSE(ir.contains("__addxf3"));
}

TEST_CASE("f80 math off x86 names compiler_rt's routines") {
    const auto aarch64{codegen::resolve_target_triple("aarch64-unknown-linux-gnu")};
    const auto x86_64{codegen::resolve_target_triple("x86_64-unknown-linux-gnu")};
    CHECK(codegen::needs_soft_f80(aarch64));
    CHECK_FALSE(codegen::needs_soft_f80(x86_64));
    CHECK(codegen::math_libcall_name(aarch64, math_function::SIN, float_format::X87) == "__sinx");
    CHECK(codegen::math_libcall_name(x86_64, math_function::SIN, float_format::X87) == "sinl");

    // So an object needing them pulls in compiler_rt
    CHECK(codegen::is_compiler_rt_symbol(aarch64, "__addxf3"));
    CHECK(codegen::is_compiler_rt_symbol(aarch64, "__floateixf"));
    CHECK(codegen::is_compiler_rt_symbol(aarch64, "__sinx"));
    CHECK_FALSE(codegen::is_compiler_rt_symbol(aarch64, "__addxf4"));
}

TEST_CASE("f80 folds at compile time on targets without x87") {
    auto [ctx, idx]{helpers::type_check_for_target(R"(
        const twice = fn(comptime x: f80): f80 { return x + x; };
        comptime {
            const third: f80 = 1.0 / 3.0;
            @assert(@bitCast(u80, third) == 0x3ffdaaaaaaaaaaaaaaab);
            @assert(@bitCast(u80, @sqrt(2.0f80)) == 0x3fffb504f333f9de6484);
            @assert(@bitCast(u80, @floatFromInt(f80, 0x3fffffffffffffff)) ==
                    0x403cfffffffffffffffc);
            @assert(@intFromFloat(u64, 1.0e19f80) == 10000000000000000000);
            @assert(twice(1.5) == 3.0);
            @assert(third < 0.5 and third > 0.25 and third != 0.0);
            @assert(@sizeOf(f80) == 16 and @alignOf(f80) == 16);
        }
    )",
                                                   "aarch64-unknown-linux-gnu")};
    helpers::check_errors<sema::diagnostics>(ctx->root_mod);
}

TEST_CASE("f80 has no C ABI off x86") {
    auto [ctx, idx]{helpers::resolve_for_target("extern const takes_f80: fn(x: f80): void;",
                                                "aarch64-unknown-linux-gnu")};
    helpers::check_errors_against<sema::diagnostics>(
        ctx->root_mod,
        sema::diagnostic{"'fn(f80): void' has no C ABI representation; extern signatures "
                         "accept 8/16/32/64-bit integers, usize/isize, bool, f32, f64, and f80 "
                         "on x86",
                         sema::error::ILLEGAL_REFERENCE_FIELD,
                         std::pair{0UZ, 24UZ}});
}

} // namespace ghoti::tests
