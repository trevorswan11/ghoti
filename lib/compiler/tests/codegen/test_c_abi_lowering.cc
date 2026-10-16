#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <stdx/types.hh>

#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "compiler/sema/analyzer.hh"
#include "compiler/sema/error.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

// The expected signatures are what clang 23 emits for the same C prototypes (tests/ffi/cabi.c),
// so these pin targets the suite can't run natively

namespace ghoti::tests {

namespace {

constexpr std::string_view AGGREGATES{R"(
    const S1 = extern struct { a: u8 };
    const S2 = extern struct { a: u8, b: u8 };
    const S3 = extern struct { a: u8, b: u8, c: u8 };
    const S6 = extern struct { a: i16, b: i16, c: i16 };
    const S8 = extern struct { a: i32, b: i32 };
    const S12 = extern struct { a: i32, b: i32, c: i32 };
    const S16 = extern struct { a: i64, b: i64 };
    const S24 = extern struct { a: i64, b: i64, c: i64 };
    const A5 = extern struct { bytes: [5]u8 };
    const F2 = extern struct { x: f32, y: f32 };
    const F3 = extern struct { x: f32, y: f32, z: f32 };
    const F4 = extern struct { x: f32, y: f32, z: f32, w: f32 };
    const D2 = extern struct { x: f64, y: f64 };
    const D3 = extern struct { x: f64, y: f64, z: f64 };
    const MixIF = extern struct { i: i32, f: f32 };
    const MixFII = extern struct { f: f32, i: i32, j: i32 };
    const MixDI = extern struct { d: f64, i: i64 };
    const Nested = extern struct { inner: extern struct { a: u8, b: u8 }, tail: u16 };
    const UIF = extern union { i: i32, f: f32 };
)"};

constexpr std::array AGGREGATE_NAMES{"S1",
                                     "S2",
                                     "S3",
                                     "S6",
                                     "S8",
                                     "S12",
                                     "S16",
                                     "S24",
                                     "A5",
                                     "F2",
                                     "F3",
                                     "F4",
                                     "D2",
                                     "D3",
                                     "MixIF",
                                     "MixFII",
                                     "MixDI",
                                     "Nested",
                                     "UIF"};

[[nodiscard]] auto exported_functions() -> std::string {
    std::string source{AGGREGATES};
    for (const std::string_view name : AGGREGATE_NAMES) {
        source += fmt::format("export const take_{0} = fn(v: {0}): i32 {{ _ = v; return 1; }};\n"
                              "export const give_{0} = fn(v: {0}): {0} {{ return v; }};\n",
                              name);
    }
    source += R"(
        export const take_i8 = fn(v: i8): i32 { return v; };
        export const take_u16 = fn(v: u16): i32 { return v; };
        export const take_bool = fn(v: bool): i32 { return if (v) 1 else 0; };
        export const give_i8 = fn(v: i32): i8 { return @truncate(i8, v); };
        export const late = fn(a: i64, b: i64, c: i64, d: i64, e: i64, f: i64, s: S16): i64 {
            return a + b + c + d + e + f + s.a;
        };
    )";
    return source;
}

[[nodiscard]] auto ir_for(std::string_view triple) -> std::string {
    const auto source{exported_functions()};
    auto [ctx, idx]{helpers::resolve_for_target(source, triple)};
    helpers::check_errors<sema::diagnostics>(ctx->root_mod);
    auto gir_mod{ctx->analyzer.emit_gir(ctx->root_mod, false)};
    REQUIRE_FALSE(ctx->root_mod.is_poisoned());
    const codegen::target_options    target_opts{.triple_str = std::string{triple},
                                                 .level      = codegen::opt_level::O0};
    const codegen::optimizer_options opt_opts{.level = codegen::opt_level::O0};
    return UNWRAP(ctx->analyzer.emit_llvm_ir_text(
        gir_mod, target_opts, opt_opts, sema::build_artifact::LIBRARY));
}

// Splits at commas outside any brackets
[[nodiscard]] auto split_top_level(std::string_view text) -> std::vector<std::string_view> {
    std::vector<std::string_view> pieces;
    i32                           depth{0};
    usize                         start{0};
    for (usize i{0}; i < text.size(); ++i) {
        const char c{text[i]};
        if (c == '(' || c == '[' || c == '{' || c == '<') { ++depth; }
        if (c == ')' || c == ']' || c == '}' || c == '>') { --depth; }
        if (c == ',' && depth == 0) {
            pieces.emplace_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < text.size()) { pieces.emplace_back(text.substr(start)); }
    for (auto& piece : pieces) {
        while (!piece.empty() && piece.front() == ' ') { piece.remove_prefix(1); }
    }
    return pieces;
}

// The leading type of an IR parameter, plus whichever ABI attributes it carries
[[nodiscard]] auto describe_param(std::string_view param) -> std::string {
    usize end{0};
    i32   depth{0};
    for (; end < param.size(); ++end) {
        const char c{param[end]};
        if (c == '(' || c == '[' || c == '{' || c == '<') { ++depth; }
        if (c == ')' || c == ']' || c == '}' || c == '>') { --depth; }
        if (c == ' ' && depth == 0) { break; }
    }
    std::string described{param.substr(0, end)};
    for (const std::string_view attr : {"byval", "sret", "signext", "zeroext", "alignstack(8)"}) {
        if (param.contains(attr)) { described += fmt::format(" {}", attr); }
    }
    return described;
}

// `ret(param, param)` for the definition of `name`, e.g. `{ i64, i32 }(i64, i32)`
[[nodiscard]] auto signature_of(std::string_view ir, std::string_view name) -> std::string {
    const auto marker{fmt::format(" @{}(", name)};
    const auto at{ir.find(marker)};
    REQUIRE(at != std::string_view::npos);
    const auto line_start{ir.rfind('\n', at) + 1};
    auto       head{ir.substr(line_start, at - line_start)};

    // The return type is the last bracket-balanced token before the name
    usize ret_start{head.size()};
    i32   depth{0};
    while (ret_start > 0) {
        const char c{head[ret_start - 1]};
        if (c == ')' || c == ']' || c == '}' || c == '>') { ++depth; }
        if (c == '(' || c == '[' || c == '{' || c == '<') { --depth; }
        if (c == ' ' && depth == 0) { break; }
        --ret_start;
    }
    std::string signature{head.substr(ret_start)};
    for (const std::string_view attr : {"signext", "zeroext"}) {
        if (head.contains(attr)) { signature = fmt::format("{} {}", attr, signature); }
    }

    const auto params_start{at + marker.size()};
    depth = 1;
    usize params_end{params_start};
    for (; params_end < ir.size() && depth > 0; ++params_end) {
        if (ir[params_end] == '(') { ++depth; }
        if (ir[params_end] == ')') { --depth; }
    }
    std::vector<std::string> params;
    for (const auto param :
         split_top_level(ir.substr(params_start, params_end - 1 - params_start))) {
        params.emplace_back(describe_param(param));
    }
    return fmt::format("{}({})", signature, fmt::join(params, ", "));
}

struct expectation {
    std::string_view function;
    std::string_view signature;
};

auto check_signatures(std::string_view triple, const std::vector<expectation>& expected) -> void {
    const auto ir{ir_for(triple)};
    for (const auto& [function, signature] : expected) {
        INFO(triple << " @" << function);
        CHECK(signature_of(ir, function) == signature);
    }
}

} // namespace

TEST_CASE("x86-64 SysV aggregates follow the eightbyte classification") {
    check_signatures("x86_64-unknown-linux-gnu",
                     {
                         {"take_S1", "i32(i8)"},
                         {"give_S1", "i8(i8)"},
                         {"take_S3", "i32(i24)"},
                         {"take_S6", "i32(i48)"},
                         {"take_S8", "i32(i64)"},
                         {"take_S12", "i32(i64, i32)"},
                         {"give_S12", "{ i64, i32 }(i64, i32)"},
                         {"take_S16", "i32(i64, i64)"},
                         {"take_S24", "i32(ptr byval)"},
                         {"give_S24", "void(ptr sret, ptr byval)"},
                         {"take_A5", "i32(i40)"},
                         {"take_F2", "i32(<2 x float>)"},
                         {"give_F3", "{ <2 x float>, float }(<2 x float>, float)"},
                         {"take_F4", "i32(<2 x float>, <2 x float>)"},
                         {"give_D2", "{ double, double }(double, double)"},
                         {"take_D3", "i32(ptr byval)"},
                         {"take_MixIF", "i32(i64)"},
                         {"give_MixFII", "{ i64, i32 }(i64, i32)"},
                         {"give_MixDI", "{ double, i64 }(double, i64)"},
                         {"take_Nested", "i32(i32)"},
                         {"take_UIF", "i32(i32)"},
                         {"take_i8", "i32(i8 signext)"},
                         {"take_u16", "i32(i16 zeroext)"},
                         {"take_bool", "i32(i1 zeroext)"},
                         {"give_i8", "signext i8(i32)"},
                         {"late", "i64(i64, i64, i64, i64, i64, i64, ptr byval)"},
                     });
}

TEST_CASE("Win64 aggregates pass as an integer or by reference") {
    check_signatures("x86_64-pc-windows-msvc",
                     {
                         {"take_S1", "i32(i8)"},
                         {"take_S2", "i32(i16)"},
                         {"take_S3", "i32(ptr)"},
                         {"give_S3", "void(ptr sret, ptr)"},
                         {"take_S8", "i32(i64)"},
                         {"give_F2", "i64(i64)"},
                         {"take_D2", "i32(ptr)"},
                         {"take_UIF", "i32(i32)"},
                         {"take_i8", "i32(i8)"},
                         {"take_bool", "i32(i1 zeroext)"},
                     });
}

TEST_CASE("AAPCS64 aggregates pass as HFAs, integer pairs, or by reference") {
    for (const auto [triple, darwin] :
         {std::pair{"aarch64-unknown-linux-gnu", false}, std::pair{"arm64-apple-macos", true}}) {
        const std::string_view hfa_attr{darwin ? "" : " alignstack(8)"};
        const auto             hfa{
            [&](std::string_view array) { return fmt::format("i32({}{})", array, hfa_attr); }};
        check_signatures(triple,
                         {
                             {"take_S1", "i32(i64)"},
                             {"give_S1", "i8(i64)"},
                             {"give_S3", "i24(i64)"},
                             {"give_S6", "i48(i64)"},
                             {"take_S12", "i32([2 x i64])"},
                             {"give_S16", "[2 x i64]([2 x i64])"},
                             {"take_S24", "i32(ptr)"},
                             {"give_S24", "void(ptr sret, ptr)"},
                             {"give_A5", "i40(i64)"},
                             {"take_F2", hfa("[2 x float]")},
                             {"take_F3", hfa("[3 x float]")},
                             {"take_D3", hfa("[3 x double]")},
                             {"take_MixDI", "i32([2 x i64])"},
                             {"take_Nested", "i32(i64)"},
                             {"take_i8", darwin ? "i32(i8 signext)" : "i32(i8)"},
                             {"take_bool", darwin ? "i32(i1 zeroext)" : "i32(i1)"},
                         });
    }
}

} // namespace ghoti::tests
