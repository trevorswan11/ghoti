#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/span>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <stdx/memory.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>

#include "compiler/codegen/c_abi.hh"
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
    const UFF = extern union { a: f32, b: f32 };
    const UFD = extern union { a: f32, d: f64 };
    const UF2 = extern union { a: [2]f32, b: f32 };
    const UF3D = extern union { a: [3]f32, d: f64 };
    const UF13 = extern union { a: f32, b: [3]f32 };
    const UDD2 = extern union { a: f64, b: [2]f64 };
    const UF4D2 = extern union { a: [4]f32, d: [2]f64 };
    const SFU = extern struct { x: f32, u: UFF };
    const SUF = extern struct { u: UFD, y: f32 };
    const SFUD = extern struct { x: f32, u: UFD };
    const UPL = extern union { p: ^u8, x: i64 };
    const SHF = struct { a: f16, b: f32 };
    const SQ = struct { x: f128 };
    const SQ2 = struct { s: SQ };
    const SI = struct { x: i128 };
    const SLI = struct { a: i64, b: i128 };
    const F1 = extern struct { x: f32 };
    const D1 = extern struct { x: f64 };
    const P1 = extern struct { p: ^u8 };
    const A3B = extern struct { a: [3]u8, b: u8 };
    const FD = extern struct { f: f32, d: f64 };
    const ID = extern struct { i: i32, d: f64 };
    const CF = extern struct { a: u8, f: f32 };
    const LI = extern struct { a: i64, b: i32 };
    const D4 = extern struct { d: [4]f64 };
    const F5 = extern struct { f: [5]f32 };
    const FI = extern struct { f: f32, i: i32 };
    const BF = extern struct { b: bool, f: f32 };
    const FA2 = extern struct { f: [2]f32 };
    const PtrF = extern struct { p: ^u8, f: f32 };
    const PF = extern packed struct { a: u8, f: f32 };
    const PFD = extern packed struct { f: f32, d: f64 };
    const AG = extern struct { f: f32, @[align(16)] g: f32 };
    const Big72 = extern struct { a: [9]i64 };
    const Big80 = extern struct { a: [20]i32 };
    const HF = struct { h: f16, f: f32 };
)"};

// `extern` aggregates take an `f80` only where C has one; `extern` rejects `f16`, `f128`, and
// `i128` everywhere, so those only reach C as fields of plain structs
constexpr std::string_view X86_AGGREGATES{R"(
    const SL = extern struct { x: f80 };
    const UL = extern union { a: f80, b: f80 };
    const ULD = extern union { a: f80, b: f64 };
    const ULI = extern union { a: f80, b: i64 };
    const UFL = extern union { a: f32, b: f80 };
)"};

constexpr std::array AGGREGATE_NAMES{
    "S1",   "S2",   "S3",   "S6",    "S8",     "S12",   "S16",    "S24", "A5",    "F2",    "F3",
    "F4",   "D2",   "D3",   "MixIF", "MixFII", "MixDI", "Nested", "UIF", "UFF",   "UFD",   "UF2",
    "UF3D", "UF13", "UDD2", "UF4D2", "SFU",    "SUF",   "SFUD",   "UPL", "SHF",   "SQ",    "SQ2",
    "SI",   "SLI",  "F1",   "D1",    "P1",     "A3B",   "FD",     "ID",  "CF",    "LI",    "D4",
    "F5",   "FI",   "BF",   "FA2",   "PtrF",   "PF",    "PFD",    "AG",  "Big72", "Big80", "HF"};

constexpr std::array X86_AGGREGATE_NAMES{"SL", "UL", "ULD", "ULI", "UFL"};

[[nodiscard]] auto take_and_give(std::string_view name) -> std::string {
    return fmt::format("export const take_{0} = fn(v: {0}): i32 {{ _ = v; return 1; }};\n"
                       "export const give_{0} = fn(v: {0}): {0} {{ return v; }};\n",
                       name);
}

[[nodiscard]] auto exported_functions(bool x86) -> std::string {
    std::string source{AGGREGATES};
    for (const std::string_view name : AGGREGATE_NAMES) { source += take_and_give(name); }
    if (x86) {
        source += X86_AGGREGATES;
        for (const std::string_view name : X86_AGGREGATE_NAMES) { source += take_and_give(name); }
    }
    source += R"(
        export const take_i8 = fn(v: i8): i32 { return v; };
        export const take_u16 = fn(v: u16): i32 { return v; };
        export const take_bool = fn(v: bool): i32 { return if (v) 1 else 0; };
        export const give_i8 = fn(v: i32): i8 { return @truncate(i8, v); };
        export const late = fn(a: i64, b: i64, c: i64, d: i64, e: i64, f: i64, s: S16): i64 {
            return a + b + c + d + e + f + s.a;
        };
        export const late_SI = fn(a: i64, b: i64, c: i64, d: i64, e: i64, s: SI): i64 {
            _ = s;
            return a + b + c + d + e;
        };
        export const take_u32 = fn(v: u32, w: i32): u32 { _ = w; return v; };
        export const many_F2 = fn(a: F2, b: F2, c: F2, d: F2, e: F2): i32 {
            _ = a; _ = b; _ = c; _ = d; _ = e;
            return 1;
        };
        export const mix_FI = fn(a: f32, b: f32, c: f32, d: f32, e: f32, f: f32, g: f32, s: FI): i32 {
            _ = a; _ = b; _ = c; _ = d; _ = e; _ = f; _ = g; _ = s;
            return 1;
        };
        export const late_FI = fn(a: i64, b: i64, c: i64, d: i64, s: FI): i32 {
            _ = a; _ = b; _ = c; _ = d; _ = s;
            return 1;
        };
        export const late_D2 = fn(a: f64, b: f64, c: f64, d: f64, e: f64, f: f64, g: f64, s: D2): i32 {
            _ = a; _ = b; _ = c; _ = d; _ = e; _ = f; _ = g; _ = s;
            return 1;
        };
        export const late_ID = fn(a: f64, b: f64, c: f64, d: f64, e: f64, f: f64, g: f64, s: ID): i32 {
            _ = a; _ = b; _ = c; _ = d; _ = e; _ = f; _ = g; _ = s;
            return 1;
        };
        extern const va_F2: fn(n: i32, ...): i32;
        export const call_va = fn(f: F2, d: D2): i32 { return va_F2(1, f, d); };
        export const many_SQ = fn(a: SQ, b: SQ, c: SQ, d: SQ, e: SQ, f: SQ, g: SQ, h: SQ, i: SQ): i32 {
            _ = a; _ = b; _ = c; _ = d; _ = e; _ = f; _ = g; _ = h; _ = i;
            return 1;
        };
    )";
    return source;
}

[[nodiscard]] auto ir_for_source(std::string_view source, std::string_view triple) -> std::string {
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

[[nodiscard]] auto ir_for(std::string_view triple) -> std::string {
    const bool x86{triple.starts_with("x86_64") || triple.starts_with("i686") ||
                   triple.starts_with("i386")};
    return ir_for_source(exported_functions(x86), triple);
}

// The constant `name` returns, from the first `ret` after its definition
[[nodiscard]] auto returned_constant(std::string_view ir, std::string_view name) -> std::string {
    const auto at{ir.find(fmt::format(" @{}(", name))};
    REQUIRE(at != std::string_view::npos);
    const auto ret{ir.find("\n  ret ", at)};
    REQUIRE(ret != std::string_view::npos);
    const auto line{stdx::string::substr(ir, ret + 7, ir.find('\n', ret + 1) - ret - 7)};
    return std::string{stdx::string::substr(line, line.rfind(' ') + 1)};
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
            pieces.emplace_back(stdx::string::substr(text, start, i - start));
            start = i + 1;
        }
    }
    if (start < text.size()) { pieces.emplace_back(stdx::string::substr(text, start)); }
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
    std::string described{stdx::string::substr(param, 0, end)};
    for (const std::string_view attr :
         {"byval", "sret", "signext", "zeroext", "alignstack(8)", "alignstack(16)"}) {
        if (param.contains(attr)) { described += fmt::format(" {}", attr); }
    }
    return described;
}

// The described parameters of the list opening just before `start`
[[nodiscard]] auto params_at(std::string_view ir, usize start) -> std::string {
    i32   depth{1};
    usize end{start};
    for (; end < ir.size() && depth > 0; ++end) {
        if (ir[end] == '(') { ++depth; }
        if (ir[end] == ')') { --depth; }
    }
    std::vector<std::string> params;
    for (const auto param : split_top_level(stdx::string::substr(ir, start, end - 1 - start))) {
        params.emplace_back(describe_param(param));
    }
    return fmt::format("{}", fmt::join(params, ", "));
}

// `ret(param, param)` for the definition of `name`, e.g. `{ i64, i32 }(i64, i32)`
[[nodiscard]] auto signature_of(std::string_view ir, std::string_view name) -> std::string {
    const auto marker{fmt::format(" @{}(", name)};
    const auto at{ir.find(marker)};
    REQUIRE(at != std::string_view::npos);
    const auto line_start{ir.rfind('\n', at) + 1};
    auto       head{stdx::string::substr(ir, line_start, at - line_start)};

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
    std::string signature{stdx::string::substr(head, ret_start)};
    for (const std::string_view attr : {"signext", "zeroext"}) {
        if (head.contains(attr)) { signature = fmt::format("{} {}", attr, signature); }
    }

    return fmt::format("{}({})", signature, params_at(ir, at + marker.size()));
}

// `text` with each named type spelled out as its body, so `%0` reads `{ float, float }`
[[nodiscard]] auto expand_named_types(std::string_view ir, std::string text) -> std::string {
    const auto is_name_char{[](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.';
    }};
    usize      at{0};
    while ((at = text.find('%', at)) != std::string::npos) {
        usize end{at + 1};
        while (end < text.size() && is_name_char(text[end])) { ++end; }
        const auto marker{fmt::format("\n{} = type ", stdx::string::substr(text, at, end - at))};
        const auto definition{ir.find(marker)};
        if (definition == std::string_view::npos) {
            at = end;
            continue;
        }
        const auto body_start{definition + marker.size()};
        text.replace(at,
                     end - at,
                     stdx::string::substr(ir, body_start, ir.find('\n', body_start) - body_start));
    }
    return text;
}

// The arguments of the first call to `callee`, e.g. `i32, [2 x i32]`
[[nodiscard]] auto call_args_of(std::string_view ir, std::string_view callee) -> std::string {
    const auto marker{fmt::format(" @{}(", callee)};
    for (auto at{ir.find(marker)}; at != std::string_view::npos; at = ir.find(marker, at + 1)) {
        const auto line_start{ir.rfind('\n', at) + 1};
        if (stdx::string::substr(ir, line_start, at - line_start).contains("call ")) {
            return params_at(ir, at + marker.size());
        }
    }
    FAIL("no call to @" << callee);
    return {};
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

using raw_shape = std::pair<std::string_view, llvm::Type*>;

[[nodiscard]] auto raw_module(llvm::LLVMContext& context, std::string_view triple)
    -> stdx::box<llvm::Module> {
    codegen::initialize_all_targets();
    const auto machine{UNWRAP(codegen::create_target_machine({.triple_str = std::string{triple}}))};
    auto       module{stdx::make_box<llvm::Module>("raw_shapes", context)};
    module->setTargetTriple(machine->getTargetTriple());
    module->setDataLayout(machine->createDataLayout());
    return module;
}

// `take_<name>(v): i32` and `give_<name>(v)` for each shape, lowered and printed
[[nodiscard]] auto lowered_take_and_give(llvm::Module&                    module,
                                         gsl::span<const raw_shape>       shapes,
                                         const codegen::union_member_map& unions = {})
    -> std::string {
    auto& context{module.getContext()};
    auto* i32{llvm::Type::getInt32Ty(context)};
    for (const auto& [name, ty] : shapes) {
        for (const bool give : {false, true}) {
            auto* fn_ty{llvm::FunctionType::get(give ? ty : i32, {ty}, false)};
            auto* fn{llvm::Function::Create(fn_ty,
                                            llvm::Function::ExternalLinkage,
                                            fmt::format("{}_{}", give ? "give" : "take", name),
                                            module)};
            llvm::IRBuilder<> builder{llvm::BasicBlock::Create(context, "entry", fn)};
            builder.CreateRet(give ? static_cast<llvm::Value*>(fn->getArg(0))
                                   : llvm::ConstantInt::get(i32, 1));
        }
    }
    codegen::lower_c_abi(module, unions);

    std::string              ir;
    llvm::raw_string_ostream out{ir};
    module.print(out, nullptr);
    return ir;
}

// Shapes ghoti can't spell yet (vectors, and unions holding an `i128` or `f128`), built as LLVM
// types and lowered directly
[[nodiscard]] auto raw_ir_for(std::string_view triple) -> std::string {
    llvm::LLVMContext context;
    const auto        module{raw_module(context, triple)};

    auto* f32{llvm::Type::getFloatTy(context)};
    auto* f64{llvm::Type::getDoubleTy(context)};
    auto* f80{llvm::Type::getX86_FP80Ty(context)};
    auto* f128{llvm::Type::getFP128Ty(context)};
    auto* i32{llvm::Type::getInt32Ty(context)};
    auto* i64{llvm::Type::getInt64Ty(context)};
    auto* i128{llvm::Type::getInt128Ty(context)};
    auto* v4f{llvm::FixedVectorType::get(f32, 4)};
    auto* v2f{llvm::FixedVectorType::get(f32, 2)};

    // Each union stores the member clang would, as the type translator lays them out
    codegen::union_member_map unions;
    const auto                union_of{[&](llvm::Type* storage, std::vector<llvm::Type*> members) {
        auto* union_ty{llvm::StructType::create(context, {storage})};
        unions[union_ty] = std::move(members);
        return union_ty;
    }};
    const std::array<raw_shape, 11> shapes{{
        {"SV", llvm::StructType::get(v4f)},
        {"SVI", llvm::StructType::get(llvm::FixedVectorType::get(i32, 4))},
        {"SV2", llvm::StructType::get(v2f, v2f)},
        {"UVD", union_of(v4f, {v4f, f64})},
        {"UID", union_of(i128, {i128, f64})},
        {"UDI2", union_of(i128, {llvm::ArrayType::get(f64, 2), i128})},
        {"UIL", union_of(i128, {i128, f80})},
        {"ULI2", union_of(f80, {f80, i128})},
        {"UQD", union_of(f128, {f128, f64})},
        {"UQL", union_of(f128, {f128, i64})},
        {"UQQ", union_of(f128, {f128, f128})},
    }};
    return lowered_take_and_give(*module, shapes, unions);
}

auto check_raw_signatures(std::string_view triple, const std::vector<expectation>& expected)
    -> void {
    const auto ir{raw_ir_for(triple)};
    for (const auto& [function, signature] : expected) {
        INFO(triple << " @" << function);
        CHECK(signature_of(ir, function) == signature);
    }
}

// Small float structs lowered under the RISC-V float ABI `abi`, as `-mabi` would pick it
auto check_flagged_signatures(std::string_view                triple,
                              std::string_view                abi,
                              const std::vector<expectation>& expected) -> void {
    llvm::LLVMContext context;
    const auto        module{raw_module(context, triple)};
    module->addModuleFlag(llvm::Module::Error, "target-abi", llvm::MDString::get(context, abi));

    auto*                          f32{llvm::Type::getFloatTy(context)};
    auto*                          f64{llvm::Type::getDoubleTy(context)};
    const std::array<raw_shape, 4> shapes{{
        {"F2", llvm::StructType::get(f32, f32)},
        {"D1", llvm::StructType::get(f64)},
        {"FD", llvm::StructType::get(f32, f64)},
        {"FI", llvm::StructType::get(f32, llvm::Type::getInt32Ty(context))},
    }};
    const auto                     ir{lowered_take_and_give(*module, shapes)};
    for (const auto& [function, signature] : expected) {
        INFO(triple << " " << abi << " @" << function);
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

TEST_CASE("x86-64 SysV unions merge their members' classes per eightbyte") {
    check_signatures("x86_64-unknown-linux-gnu",
                     {
                         {"take_UFF", "i32(float)"},
                         {"give_UFF", "float(float)"},
                         {"take_UFD", "i32(double)"},
                         {"take_UF2", "i32(<2 x float>)"},
                         {"take_UF3D", "i32(double, double)"},
                         {"give_UF3D", "{ double, double }(double, double)"},
                         {"take_UF13", "i32(<2 x float>, float)"},
                         {"give_UF13", "{ <2 x float>, float }(<2 x float>, float)"},
                         {"take_UDD2", "i32(double, double)"},
                         {"take_UF4D2", "i32(double, double)"},
                         {"take_SFU", "i32(<2 x float>)"},
                         {"give_SUF", "{ double, float }(double, float)"},
                         {"give_SFUD", "{ float, double }(float, double)"},
                         {"take_UPL", "i32(ptr)"},
                         {"take_UIF", "i32(i32)"},
                         {"give_SHF", "<4 x half>(<4 x half>)"},
                     });
}

TEST_CASE("x86-64 SysV f80, f128, and i128 fields take their two-eightbyte classes") {
    check_signatures("x86_64-unknown-linux-gnu",
                     {
                         {"take_SL", "i32(ptr byval)"},
                         {"give_SL", "x86_fp80(ptr byval)"},
                         {"give_UL", "x86_fp80(ptr byval)"},
                         {"take_ULD", "i32(ptr byval)"},
                         {"give_ULD", "void(ptr sret, ptr byval)"},
                         {"give_ULI", "void(ptr sret, ptr byval)"},
                         {"give_UFL", "void(ptr sret, ptr byval)"},
                         {"give_SQ", "fp128(fp128)"},
                         {"give_SQ2", "fp128(fp128)"},
                         {"give_SI", "i128(i128)"},
                         {"give_SLI", "void(ptr sret, ptr byval)"},
                         {"late_SI", "i64(i64, i64, i64, i64, i64, ptr byval)"},
                         {"many_SQ",
                          "i32(fp128, fp128, fp128, fp128, fp128, fp128, fp128, fp128, "
                          "ptr byval)"},
                     });
}

TEST_CASE("x86-64 Darwin passes an f80 high half beside an integer as SSE") {
    check_signatures("x86_64-apple-macos",
                     {
                         {"take_ULI", "i32(i64, double)"},
                         {"give_ULI", "{ i64, double }(i64, double)"},
                         {"give_ULD", "void(ptr sret, ptr byval)"},
                         {"give_SL", "x86_fp80(ptr byval)"},
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
                         {"take_UFD", "i32(i64)"},
                         {"take_UF13", "i32(ptr)"},
                         {"give_UF3D", "void(ptr sret, ptr)"},
                         {"take_SI", "i32(ptr)"},
                         {"take_SQ", "i32(ptr)"},
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

TEST_CASE("AAPCS64 unions of one float type are HFAs") {
    for (const auto [triple, darwin] :
         {std::pair{"aarch64-unknown-linux-gnu", false}, std::pair{"arm64-apple-macos", true}}) {
        const auto hfa{[&](std::string_view array, std::string_view align) {
            return fmt::format(
                "i32({}{})", array, darwin ? "" : fmt::format(" alignstack({})", align));
        }};
        check_signatures(triple,
                         {
                             {"take_UFF", hfa("[1 x float]", "8")},
                             {"take_UF2", hfa("[2 x float]", "8")},
                             {"take_UF13", hfa("[3 x float]", "8")},
                             {"take_UDD2", hfa("[2 x double]", "8")},
                             {"take_SFU", hfa("[2 x float]", "8")},
                             {"give_UFD", "i64(i64)"},
                             {"give_UF3D", "[2 x i64]([2 x i64])"},
                             {"take_UF4D2", "i32([2 x i64])"},
                             {"take_SUF", "i32([2 x i64])"},
                             {"take_SFUD", "i32([2 x i64])"},
                             {"take_SQ", hfa("[1 x fp128]", "16")},
                             {"take_SQ2", hfa("[1 x fp128]", "16")},
                             {"give_SI", "i128(i128)"},
                             {"late_SI", "i64(i64, i64, i64, i64, i64, i128)"},
                         });
    }
}

TEST_CASE("16-byte vectors and i128 or f128 unions classify as clang does") {
    check_raw_signatures("x86_64-unknown-linux-gnu",
                         {
                             {"take_SV", "i32(<4 x float>)"},
                             {"give_SV", "<4 x float>(<4 x float>)"},
                             {"give_SVI", "<4 x i32>(<4 x i32>)"},
                             {"give_SV2", "{ double, double }(double, double)"},
                             {"give_UVD", "<2 x double>(<2 x double>)"},
                             {"take_UID", "i32(i128)"},
                             {"take_UDI2", "i32(i128)"},
                             {"give_UIL", "i128(i128)"},
                             {"give_ULI2", "{ i64, i64 }(i64, i64)"},
                             {"give_UQD", "<2 x double>(<2 x double>)"},
                             {"give_UQL", "{ i64, double }(i64, double)"},
                             {"take_UQQ", "i32(<2 x double>)"},
                         });
    check_raw_signatures("aarch64-unknown-linux-gnu",
                         {
                             {"take_SV", "i32([1 x <4 x float>] alignstack(16))"},
                             {"take_SVI", "i32([1 x <4 x i32>] alignstack(16))"},
                             {"take_SV2", "i32([2 x <2 x float>] alignstack(8))"},
                             {"give_UVD", "i128(i128)"},
                             {"take_UID", "i32(i128)"},
                             {"give_UQD", "i128(i128)"},
                             {"take_UQL", "i32(i128)"},
                         });
    check_raw_signatures("arm64-apple-macos",
                         {
                             {"take_SV", "i32([1 x <4 x float>])"},
                             {"give_UVD", "i128(i128)"},
                             {"take_UID", "i32(i128)"},
                         });
}

TEST_CASE("i386 expands padding-free structs and passes the rest byval") {
    for (const auto* triple : {"i686-unknown-linux-gnu", "i686-pc-windows-msvc"}) {
        check_signatures(triple,
                         {
                             {"take_S1", "i32(ptr byval)"},
                             {"take_S8", "i32(i32, i32)"},
                             {"take_S16", "i32(i64, i64)"},
                             {"take_S24", "i32(ptr byval)"},
                             {"take_F4", "i32(float, float, float, float)"},
                             {"take_D2", "i32(double, double)"},
                             {"take_D3", "i32(ptr byval)"},
                             {"take_MixIF", "i32(i32, float)"},
                             {"take_MixDI", "i32(double, i64)"},
                             {"take_Nested", "i32(ptr byval)"},
                             {"take_UIF", "i32(ptr byval)"},
                             {"take_P1", "i32(ptr)"},
                             {"take_A3B", "i32(ptr byval)"},
                             {"take_CF", "i32(ptr byval)"},
                             {"take_PFD", "i32(float, double)"},
                             {"take_Big80", "i32(ptr byval)"},
                             {"late", "i64(i64, i64, i64, i64, i64, i64, i64, i64)"},
                             {"take_i8", "i32(i8 signext)"},
                             {"take_u16", "i32(i16 zeroext)"},
                             {"take_bool", "i32(i1 zeroext)"},
                             {"give_i8", "signext i8(i32)"},
                         });
    }
}

TEST_CASE("i386 Linux returns every aggregate in memory") {
    check_signatures("i686-unknown-linux-gnu",
                     {
                         {"give_S1", "void(ptr sret, ptr byval)"},
                         {"give_S8", "void(ptr sret, i32, i32)"},
                         {"give_F1", "void(ptr sret, float)"},
                         {"give_P1", "void(ptr sret, ptr)"},
                         {"take_AG", "i32(ptr byval)"},
                     });
}

TEST_CASE("i386 Windows, macOS, and the BSDs return register-sized structs in registers") {
    for (const std::string_view triple : {"i686-pc-windows-msvc",
                                          "i686-w64-windows-gnu",
                                          "i386-apple-macos",
                                          "i686-unknown-freebsd"}) {
        const bool msvc{triple.ends_with("msvc")};
        check_signatures(triple,
                         {
                             {"give_S1", "i8(ptr byval)"},
                             {"give_S2", "i16(ptr byval)"},
                             {"give_S3", "void(ptr sret, ptr byval)"},
                             {"give_S8", "i64(i32, i32)"},
                             {"give_S12", "void(ptr sret, i32, i32, i32)"},
                             {"give_F2", "i64(float, float)"},
                             {"give_Nested", "i32(ptr byval)"},
                             {"give_UIF", "i32(ptr byval)"},
                             {"give_UFD", "i64(ptr byval)"},
                             {"give_UPL", "i64(ptr byval)"},
                             {"give_SFU", "i64(ptr byval)"},
                             {"give_CF", "i64(ptr byval)"},
                             {"give_A3B", "void(ptr sret, ptr byval)"},
                             {"give_P1", "ptr(ptr)"},
                             {"give_F1", msvc ? "i32(float)" : "float(float)"},
                             {"give_D1", msvc ? "i64(double)" : "double(double)"},
                         });
    }
}

TEST_CASE("i386 SysV expands structs whose f64 and i64 fields sit at 4-byte offsets") {
    for (const auto* triple :
         {"i686-unknown-linux-gnu", "i686-unknown-freebsd", "i386-apple-macos"}) {
        check_signatures(triple,
                         {
                             {"take_FD", "i32(float, double)"},
                             {"take_ID", "i32(i32, double)"},
                             {"take_LI", "i32(i64, i32)"},
                             {"take_SFUD", "i32(ptr byval)"},
                             {"give_FD", "void(ptr sret, float, double)"},
                             {"late_ID",
                              "i32(double, double, double, double, double, double, double, i32, "
                              "double)"},
                         });
    }
}

TEST_CASE("Struct layout aligns f64, i64, and f80 as the target's C does") {
    constexpr std::string_view source{R"(
        const FD = extern struct { f: f32, d: f64 };
        const LI = extern struct { a: i64, b: i32 };
        const CL = extern struct { c: u8, x: f80 };
        export const size_FD = fn(): usize { return @sizeOf(FD); };
        export const align_FD = fn(): usize { return @alignOf(FD); };
        export const size_LI = fn(): usize { return @sizeOf(LI); };
        export const align_i64 = fn(): usize { return @alignOf(i64); };
        export const size_f80 = fn(): usize { return @sizeOf(f80); };
        export const size_CL = fn(): usize { return @sizeOf(CL); };
        export const align_CL = fn(): usize { return @alignOf(CL); };
    )"};

    struct layout {
        std::string_view triple;
        std::string_view fd_size, fd_align, li_size, i64_align, f80_size, cl_size, cl_align;
    };
    // clang 23's sizeof / _Alignof for the same C structs with f80 as `long double`, except under
    // MSVC, where `long double` is a double and f80 follows the data layout's `f80:128`
    for (const auto& want : {
             layout{"i686-unknown-linux-gnu", "12", "4", "12", "4", "12", "16", "4"},
             layout{"i686-unknown-freebsd", "12", "4", "12", "4", "12", "16", "4"},
             layout{"i386-apple-macos", "12", "4", "12", "4", "16", "32", "16"},
             layout{"i686-w64-windows-gnu", "16", "8", "16", "8", "12", "16", "4"},
             layout{"i686-pc-windows-msvc", "16", "8", "16", "8", "16", "32", "16"},
             layout{"x86_64-unknown-linux-gnu", "16", "8", "16", "8", "16", "32", "16"},
         }) {
        INFO(want.triple);
        const auto ir{ir_for_source(source, want.triple)};
        CHECK(returned_constant(ir, "size_FD") == want.fd_size);
        CHECK(returned_constant(ir, "align_FD") == want.fd_align);
        CHECK(returned_constant(ir, "size_LI") == want.li_size);
        CHECK(returned_constant(ir, "align_i64") == want.i64_align);
        CHECK(returned_constant(ir, "size_f80") == want.f80_size);
        CHECK(returned_constant(ir, "size_CL") == want.cl_size);
        CHECK(returned_constant(ir, "align_CL") == want.cl_align);
        // The lowered struct takes the data layout's own f64 alignment, with no inserted padding
        CHECK_FALSE(ir.contains("float, [4 x i8], double"));
    }
}

TEST_CASE("MSVC i386 keeps padded and over-aligned structs whole") {
    check_signatures("i686-pc-windows-msvc",
                     {
                         {"take_FD", "i32(ptr byval)"},
                         {"take_ID", "i32(ptr byval)"},
                         {"take_LI", "i32(ptr byval)"},
                         {"take_AG", "i32(ptr)"},
                         {"give_AG", "void(ptr sret, ptr)"},
                         {"late_ID",
                          "i32(double, double, double, double, double, double, double, ptr "
                          "byval)"},
                     });
}

TEST_CASE("AAPCS passes aggregates as integer arrays and returns at most a word") {
    for (const auto* triple : {"armv7-unknown-linux-gnueabi", "armv7-unknown-linux-gnueabihf"}) {
        check_signatures(triple,
                         {
                             {"take_S1", "i32([1 x i32])"},
                             {"give_S1", "i8([1 x i32])"},
                             {"give_S2", "i16([1 x i32])"},
                             {"give_S3", "i32([1 x i32])"},
                             {"take_S6", "i32([2 x i32])"},
                             {"give_S6", "void(ptr sret, [2 x i32])"},
                             {"take_S16", "i32([2 x i64])"},
                             {"take_S24", "i32([3 x i64])"},
                             {"take_MixIF", "i32([2 x i32])"},
                             {"give_MixIF", "void(ptr sret, [2 x i32])"},
                             {"take_UFD", "i32([1 x i64])"},
                             {"take_UPL", "i32([1 x i64])"},
                             {"give_P1", "i32([1 x i32])"},
                             {"take_F5", "i32([5 x i32])"},
                             {"take_AG", "i32([4 x i64])"},
                             {"take_Big72", "i32(ptr byval)"},
                             {"take_Big80", "i32(ptr byval)"},
                             {"late", "i64(i64, i64, i64, i64, i64, i64, [2 x i64])"},
                             {"take_i8", "i32(i8 signext)"},
                             {"take_bool", "i32(i1 zeroext)"},
                             {"give_i8", "signext i8(i32)"},
                         });
    }
    check_signatures(
        "armv7-unknown-linux-gnueabi",
        {
            {"take_F2", "i32([2 x i32])"},
            {"give_F1", "i32([1 x i32])"},
            {"take_D2", "i32([2 x i64])"},
            {"give_D1", "void(ptr sret, [1 x i64])"},
            {"mix_FI", "i32(float, float, float, float, float, float, float, [2 x i32])"},
        });
}

TEST_CASE("AAPCS-VFP passes HFAs as themselves, except to variadic calls") {
    const auto ir{ir_for("armv7-unknown-linux-gnueabihf")};
    for (const auto& [function, signature] : std::vector<expectation>{
             {"take_F2", "i32({ float, float })"},
             {"give_F2", "{ float, float }({ float, float })"},
             {"take_D1", "i32({ double })"},
             {"give_D1", "{ double }({ double })"},
             {"take_D4", "i32({ [4 x double] })"},
             {"take_SFU", "i32({ float, { float } })"},
             {"give_UFF", "{ float }({ float })"},
             {"take_FA2", "i32({ [2 x float] })"},
             {"give_BF", "void(ptr sret, [2 x i32])"},
             {"mix_FI", "i32(float, float, float, float, float, float, float, [2 x i32])"},
             {"late_D2",
              "i32(double, double, double, double, double, double, double, { double, double })"},
         }) {
        INFO("@" << function);
        CHECK(expand_named_types(ir, signature_of(ir, function)) == signature);
    }
    CHECK(call_args_of(ir, "va_F2") == "i32, [2 x i32], [2 x i64]");
}

TEST_CASE("RISC-V LP64D flattens float structs into register pairs") {
    check_signatures(
        "riscv64-unknown-linux-gnu",
        {
            {"take_S1", "signext i32(i8)"},
            {"give_S3", "i24(i24)"},
            {"take_S6", "signext i32(i48)"},
            {"take_S12", "signext i32([2 x i64])"},
            {"give_S16", "[2 x i64]([2 x i64])"},
            {"take_S24", "signext i32(ptr)"},
            {"give_S24", "void(ptr sret, ptr)"},
            {"take_A5", "signext i32(i40)"},
            {"take_F2", "signext i32(float, float)"},
            {"give_F2", "{ float, float }(float, float)"},
            {"take_F3", "signext i32([2 x i64])"},
            {"give_D2", "{ double, double }(double, double)"},
            {"take_D3", "signext i32(ptr)"},
            {"give_MixIF", "{ i32, float }(i32, float)"},
            {"give_MixDI", "{ double, i64 }(double, i64)"},
            {"take_MixFII", "signext i32([2 x i64])"},
            {"take_UFF", "signext i32(i32)"},
            {"take_UF2", "signext i32(i64)"},
            {"give_F1", "float(float)"},
            {"give_D1", "double(double)"},
            {"take_P1", "signext i32(i64)"},
            {"give_FD", "{ float, double }(float, double)"},
            {"take_CF", "signext i32(i8, float)"},
            {"take_BF", "signext i32(i1, float)"},
            {"take_FA2", "signext i32(float, float)"},
            {"take_PtrF", "signext i32([2 x i64])"},
            {"give_PF", "<{ i8, float }>(i8, float)"},
            {"give_PFD", "<{ float, double }>(float, double)"},
            {"take_AG", "signext i32(float, float)"},
            {"give_AG", "{ float, float }(float, float)"},
            {"take_HF", "signext i32(half, float)"},
            {"take_D4", "signext i32(ptr)"},
            {"many_F2", "signext i32(float, float, float, float, float, float, float, float, i64)"},
            {"late_FI", "signext i32(i64, i64, i64, i64, float, i32)"},
            {"late_D2",
             "signext i32(double, double, double, double, double, double, double, [2 x i64])"},
            {"late_ID",
             "signext i32(double, double, double, double, double, double, double, i32, double)"},
            {"take_u32", "signext i32(i32 signext, i32 signext)"},
            {"take_i8", "signext i32(i8 signext)"},
            {"give_i8", "signext i8(i32 signext)"},
        });
}

TEST_CASE("RISC-V without float registers passes structs as integers") {
    check_signatures("riscv64-unknown-elf",
                     {
                         {"take_F2", "signext i32(i64)"},
                         {"give_D2", "[2 x i64]([2 x i64])"},
                         {"take_F1", "signext i32(i32)"},
                         {"give_D1", "i64(i64)"},
                         {"take_MixDI", "signext i32([2 x i64])"},
                         {"take_FD", "signext i32([2 x i64])"},
                     });
    check_signatures("riscv32-unknown-elf",
                     {
                         {"take_S6", "i32([2 x i32])"},
                         {"take_S8", "i32([2 x i32])"},
                         {"take_S12", "i32(ptr)"},
                         {"give_S12", "void(ptr sret, ptr)"},
                         {"take_UFD", "i32(i64)"},
                         {"take_F2", "i32([2 x i32])"},
                         {"take_D1", "i32(i64)"},
                         {"late", "i64(i64, i64, i64, i64, i64, i64, ptr)"},
                     });
}

TEST_CASE("RISC-V ILP32D flattens within the 32-bit integer limit") {
    check_signatures(
        "riscv32-unknown-linux-gnu",
        {
            {"take_F2", "i32(float, float)"},
            {"give_D2", "{ double, double }(double, double)"},
            {"take_D1", "i32(double)"},
            {"take_FD", "i32(float, double)"},
            {"take_MixDI", "i32(ptr)"},
            {"take_LI", "i32(ptr)"},
            {"take_PFD", "i32(float, double)"},
            {"many_F2", "i32(float, float, float, float, float, float, float, float, [2 x i32])"},
            {"take_u32", "i32(i32, i32)"},
        });
}

TEST_CASE("RISC-V takes its float ABI from the target-abi flag") {
    CHECK(ir_for("riscv64-unknown-linux-gnu").contains(R"(!"target-abi", !"lp64d")"));
    CHECK(ir_for("riscv32-unknown-elf").contains(R"(!"target-abi", !"ilp32")"));

    check_flagged_signatures("riscv64-unknown-elf",
                             "lp64f",
                             {
                                 {"take_FD", "i32([2 x i64])"},
                                 {"take_FI", "i32(float, i32)"},
                                 {"take_D1", "i32(i64)"},
                                 {"give_F2", "{ float, float }(float, float)"},
                             });
    check_flagged_signatures("riscv32-unknown-elf",
                             "ilp32f",
                             {
                                 {"take_F2", "i32(float, float)"},
                                 {"take_D1", "i32(i64)"},
                                 {"take_FD", "i32(ptr)"},
                             });
    check_flagged_signatures("riscv32-unknown-elf",
                             "ilp32e",
                             {
                                 {"take_F2", "i32([2 x i32])"},
                                 {"take_D1", "i32(i64)"},
                                 {"take_FI", "i32([2 x i32])"},
                             });
}

} // namespace ghoti::tests
