#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/TargetParser/Triple.h>

#include "compiler/codegen/target.hh"
#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"
#include "support/tempfile.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view visible_library{R"(
@[visibility(.hidden)] export const helper := fn(): i32 { return 1; };
export const api := fn(): i32 { return helper() + 1; };
@[visibility(.hidden)] export var hidden_state: i32 = 3;
export var shared_state: i32 = 4;
)"};

[[nodiscard]] auto ir_for(std::string_view source, std::string_view triple) -> std::string {
    auto [ctx, idx]{helpers::resolve_for_target(source, triple)};
    helpers::check_errors<sema::diagnostics>(ctx->root_mod);
    llvm::LLVMContext context;
    auto              llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    return helpers::ir_text(*llvm_mod);
}

} // namespace

TEST_CASE("visibility lowers onto functions and globals") {
    const auto ir{ir_for(R"(
        @[visibility(.hidden)] export const hidden_fn := fn(): i32 { return 1; };
        @[visibility(.protected)] pub const protected_fn := fn(): i32 { return 2; };
        @[visibility(.default)] export const default_fn := fn(): i32 { return 3; };
        @[visibility(.hidden)] export var hidden_var: i32 = 1;
        @[visibility(.protected)] pub var protected_var: i32 = 2;
    )",
                         "x86_64-unknown-linux-gnu")};
    CHECK(ir.contains("define hidden i32 @hidden_fn("));
    CHECK(ir.contains("define protected i32 @"));
    CHECK(ir.contains("define i32 @default_fn("));
    CHECK(ir.contains("@hidden_var = hidden global i32 1"));
    CHECK(ir.contains("protected global i32 2"));
}

TEST_CASE("visibility combines with weak and extern linkage") {
    const auto ir{ir_for(R"(
        @[visibility(.hidden)] export weak const fallback := fn(): i32 { return 1; };
        @[visibility(.hidden)] extern("c") const imported: fn(): i32;
        pub const use := fn(): i32 { return fallback() + imported(); };
    )",
                         "x86_64-unknown-linux-gnu")};
    CHECK(ir.contains("define weak hidden i32 @fallback("));
    CHECK(ir.contains("declare hidden i32 @imported("));
}

TEST_CASE("@typeInfo of a function declaration reflects its visibility") {
    helpers::resolve_and_check(R"(
        @[visibility(.hidden)] export const quiet := fn(): i32 { return 1; };
        export const loud := fn(): i32 { return 2; };
        constexpr {
            @assert(@typeInfo(quiet).function.visibility == .hidden);
            @assert(@typeInfo(loud).function.visibility == .default);
            @assert(@typeInfo(@TypeOf(quiet)).function.visibility == .default);
        }
    )");
}

TEST_CASE("visibility needs a symbol other objects can link to") {
    constexpr std::string_view message{
        "Attribute 'visibility' needs a symbol other objects can link to: a 'pub', 'export', "
        "'extern', or 'weak' declaration"};
    auto [ctx, idx]{helpers::resolve_for_target(R"(@[visibility(.hidden)] const internal := 1;
@[visibility(.hidden)] const private_fn := fn(): void {};
pub const f := fn(): void {
    @[visibility(.hidden)] var local: i32 = 0;
    _ = local;
};
)",
                                                "x86_64-unknown-linux-gnu")};
    helpers::check_errors_against<sema::diagnostics>(
        ctx->root_mod,
        sema::diagnostic{std::string{message}, sema::error::ILLEGAL_ATTRIBUTE, std::pair{0UZ, 2UZ}},
        sema::diagnostic{std::string{message}, sema::error::ILLEGAL_ATTRIBUTE, std::pair{1UZ, 2UZ}},
        sema::diagnostic{
            std::string{message}, sema::error::ILLEGAL_ATTRIBUTE, std::pair{3UZ, 6UZ}});
}

TEST_CASE("Protected visibility is rejected outside ELF") {
    for (const std::string_view triple : {"x86_64-w64-windows-gnu",
                                          "x86_64-pc-windows-msvc",
                                          "arm64-apple-macos",
                                          "wasm32-unknown-unknown"}) {
        INFO(triple);
        auto [ctx, idx]{
            helpers::resolve_for_target("@[visibility(.protected)] pub var g: i32 = 0;", triple)};
        helpers::check_errors_against<sema::diagnostics>(
            ctx->root_mod,
            sema::diagnostic{fmt::format("Visibility '.protected' needs an ELF target; '{}' has no "
                                         "protected symbols",
                                         codegen::resolve_target_triple(triple).str()),
                             sema::error::ILLEGAL_ATTRIBUTE,
                             std::pair{0UZ, 13UZ}});
    }
}

TEST_CASE("A hidden symbol stays out of a shared library's exports") {
    for (const std::string_view triple : {"x86_64-unknown-linux-gnu",
                                          "aarch64-unknown-linux-gnu",
                                          "x86_64-w64-windows-gnu",
                                          "x86_64-pc-windows-msvc",
                                          "arm64-apple-macos"}) {
        INFO(triple);
        const codegen::target_options target_opts{.triple_str = std::string{triple}};
        auto [ctx, idx]{helpers::resolve_for_target(visible_library, triple)};
        helpers::check_errors<sema::diagnostics>(ctx->root_mod);

        llvm::LLVMContext context;
        const tempfile    library{"visibility_library"};
        const auto emitted{helpers::emit_dynamic_lib(*ctx, context, library.path, target_opts)};
        if (!emitted) { fmt::println(std::cerr, "{}", emitted.error()); }
        REQUIRE(emitted);

        const auto exports{helpers::exported_symbols(library.path, target_opts)};
        CHECK(std::ranges::contains(exports, "api"));
        CHECK(std::ranges::contains(exports, "shared_state"));
        CHECK_FALSE(std::ranges::contains(exports, "helper"));
        CHECK_FALSE(std::ranges::contains(exports, "hidden_state"));
    }
}

} // namespace ghoti::tests
