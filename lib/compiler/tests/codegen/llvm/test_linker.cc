#include <filesystem>

#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/LLVMContext.h>
#include <stdx/memory.hh>
#include <stdx/result.hh>

#include "compiler/codegen/llvm_scope.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/bin_utils.hh"
#include "support/tempfile.hh"

namespace ghoti::tests {

TEST_CASE("In-process LLD linker execution") {
    codegen::llvm_scope scope;

    constexpr auto input = R"(
        pub const main := fn(args: [][:0]u8): void {
            return;
        };
    )";

    SECTION("Emit and link cross-target ELF executable for Linux x86_64") {
        llvm::LLVMContext context;
        auto [ctx, idx]{helpers::resolve_and_check(input)};
        REQUIRE(ctx->analyzer.validate_main_entry(ctx->root_mod));

        tempfile                out_file{"test_linux_exe"};
        codegen::target_options target_opts{
            .triple_str = "x86_64-unknown-linux-gnu",
            .level      = codegen::opt_level::O2,
        };

        CHECK(helpers::emit_executable(*ctx, context, out_file, target_opts));
        CHECK(std::filesystem::exists(out_file));
        CHECK(std::filesystem::file_size(out_file) > 0);
        CHECK(bin_utils::check_elf_header(out_file));
    }
}

} // namespace ghoti::tests
