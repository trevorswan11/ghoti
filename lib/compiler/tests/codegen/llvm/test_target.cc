#include <filesystem>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>
#include <magic_enum/magic_enum.hpp>
#include <stdx/enum.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/llvm_scope.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/bin_utils.hh"
#include "support/tempfile.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("Target initialization and triple resolution") {
    codegen::llvm_scope scope;

    SECTION("Default triple resolves to host") {
        const auto default_triple{codegen::resolve_target_triple()};
        CHECK_FALSE(default_triple.empty());
    }

    SECTION("Explicit triples are normalized") {
        const auto  linux_triple{codegen::resolve_target_triple("x86_64-unknown-linux-gnu")};
        const auto& linux_triple_str{linux_triple.str()};
        CHECK(linux_triple_str.contains("x86_64"));
        CHECK(linux_triple_str.contains("linux"));

        const auto arm_triple{codegen::resolve_target_triple("aarch64-unknown-linux-gnu")};
        const auto arm_triple_str{arm_triple.str()};
        CHECK(arm_triple_str.contains("aarch64"));
    }

    SECTION("Windows triples default to GNU unless MSVC is explicitly requested") {
        const auto win_plain{codegen::resolve_target_triple("x86_64-windows")};
        CHECK(win_plain.isOSWindows());
        CHECK(win_plain.isGNUEnvironment());

        const auto win_gnu{codegen::resolve_target_triple("x86_64-w64-windows-gnu")};
        CHECK(win_gnu.isOSWindows());
        CHECK(win_gnu.isGNUEnvironment());

        const auto win_msvc{codegen::resolve_target_triple("x86_64-pc-windows-msvc")};
        CHECK(win_msvc.isOSWindows());
        CHECK(win_msvc.getEnvironment() == llvm::Triple::MSVC);
    }
}

TEST_CASE("Target machine creation across architectures") {
    codegen::llvm_scope scope;
    codegen::initialize_all_targets();

    SECTION("Host target machine creation") {
        codegen::target_options opts;
        REQUIRE(UNWRAP(codegen::create_target_machine(opts)));
    }

    SECTION("Cross-target machine creation for enabled targets") {
        const std::string triple{GENERATE("x86_64-unknown-linux-gnu",
                                          "x86_64-w64-windows-gnu",
                                          "aarch64-unknown-linux-gnu",
                                          "armv7-unknown-linux-gnueabihf",
                                          "riscv64-unknown-linux-gnu",
                                          "wasm32-unknown-unknown")};

        codegen::target_options opts{.triple_str = triple};
        auto                    tm{UNWRAP(codegen::create_target_machine(opts))};
        CHECK(tm->getTargetTriple().str().contains(stdx::string::substr(triple, 0, 4)));
    }

    SECTION("Invalid target triple returns error") {
        codegen::target_options opts{.triple_str = "invalid-unknown-target-triple"};
        CHECK(UNWRAP_ERR(codegen::create_target_machine(opts)).get_error() ==
              codegen::error::TARGET_LOOKUP_FAILED);
    }
}

TEST_CASE("CodeGen OptLevel translation") {
    using codegen::opt_level;

    CHECK(codegen::to_llvm_codegen_opt_level(opt_level::O0) == llvm::CodeGenOptLevel::None);
    CHECK(codegen::to_llvm_codegen_opt_level(opt_level::O1) == llvm::CodeGenOptLevel::Less);
    CHECK(codegen::to_llvm_codegen_opt_level(opt_level::O2) == llvm::CodeGenOptLevel::Default);
    CHECK(codegen::to_llvm_codegen_opt_level(opt_level::O3) == llvm::CodeGenOptLevel::Aggressive);
    CHECK(codegen::to_llvm_codegen_opt_level(opt_level::Os) == llvm::CodeGenOptLevel::Default);
    CHECK(codegen::to_llvm_codegen_opt_level(opt_level::Oz) == llvm::CodeGenOptLevel::Default);
}

TEST_CASE("Object file emission") {
    constexpr auto input = R"(
        pub const add := fn(a: i64, b: i64): i64 {
            return a + b;
        };

        pub const multiply := fn(a: i64, b: i64): i64 {
            return a * b;
        };
    )";

    SECTION("Emit host object file") {
        codegen::llvm_scope scope;
        llvm::LLVMContext        context;
        auto [ctx, idx]{helpers::resolve_and_check(input)};

        tempfile                   f{"test_output_host.o"};
        codegen::target_options    target_opts{.level = codegen::opt_level::O2};
        codegen::optimizer_options opt_opts{.level = codegen::opt_level::O2};

        REQUIRE(helpers::emit_object(*ctx, context, f.path, target_opts, opt_opts));
        CHECK(std::filesystem::exists(f.path));
        CHECK(std::filesystem::file_size(f.path) > 0);
    }

    SECTION("Emit cross-target object file for Linux x86_64") {
        codegen::llvm_scope scope;
        llvm::LLVMContext        context;
        auto [ctx, idx]{helpers::resolve_and_check(input)};

        tempfile                f{"test_output_linux_x86_64.o"};
        codegen::target_options target_opts{
            .triple_str = "x86_64-unknown-linux-gnu",
            .level      = codegen::opt_level::O2,
        };

        REQUIRE(helpers::emit_object(*ctx, context, f.path, target_opts));
        CHECK(std::filesystem::exists(f.path));
        CHECK(std::filesystem::file_size(f.path) > 0);
        CHECK(bin_utils::check_elf_header(f.path));
    }

    SECTION("Emit cross-target object file for Windows x86_64 MinGW") {
        codegen::llvm_scope scope;
        llvm::LLVMContext        context;
        auto [ctx, idx]{helpers::resolve_and_check(input)};

        tempfile                f{"test_output_windows_x86_64.o"};
        codegen::target_options target_opts{
            .triple_str = "x86_64-w64-windows-gnu",
            .level      = codegen::opt_level::O1,
        };

        REQUIRE(helpers::emit_object(*ctx, context, f.path, target_opts));
        CHECK(std::filesystem::exists(f.path));
        CHECK(std::filesystem::file_size(f.path) > 0);
    }
}

} // namespace ghoti::tests
