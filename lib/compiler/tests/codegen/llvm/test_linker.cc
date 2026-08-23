#include <array>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/LLVMContext.h>
#include <magic_enum/magic_enum.hpp>
#include <stdx/enum.hh>
#include <stdx/harness/hooks.hh>
#include <stdx/memory.hh>
#include <stdx/result.hh>

#include "compiler/codegen/llvm_scope.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/tempfile.hh"

namespace ghoti::tests {

TEST_CASE("In-process LLD execution for main entry point across targets and optimization levels") {
    codegen::llvm_scope   scope;
    stdx::untracked_scope untracked_guard;

    // No `args` param, so this links even without a real sysroot for the target.
    constexpr auto input = R"(
        pub const main := fn(): i32 {
            return 0;
        };
    )";

    constexpr std::array target_triples{
        "x86_64-unknown-linux-gnu",
        "aarch64-unknown-linux-gnu",
        "x86_64-w64-windows-gnu",
        "x86_64-pc-windows-msvc",
        "x86_64-apple-darwin",
        "arm64-apple-darwin",
        "wasm32-unknown-unknown",
    };

    for (const auto triple_str : target_triples) {
        for (const auto level : stdx::enum_range<codegen::opt_level>()) {
            DYNAMIC_SECTION("Target: " << triple_str << " Opt: " << magic_enum::enum_name(level)) {
                llvm::LLVMContext context;
                auto [ctx, idx]{helpers::resolve_and_check(input)};
                REQUIRE(ctx->analyzer.validate_main_entry(ctx->root_mod));

                tempfile                out_file{"test_e2e_exe"};
                codegen::target_options target_opts{
                    .triple_str = triple_str,
                    .level      = level,
                };
                codegen::optimizer_options opt_opts{
                    .level = level,
                };

                CHECK(helpers::emit_executable(*ctx, context, out_file, target_opts, opt_opts));
                CHECK(std::filesystem::exists(out_file));
                CHECK(std::filesystem::file_size(out_file) > 0);
            }
        }
    }
}

} // namespace ghoti::tests
