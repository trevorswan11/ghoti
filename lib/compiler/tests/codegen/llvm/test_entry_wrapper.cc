#include <string>

#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <stdx/memory.hh>
#include <stdx/result.hh>

#include "compiler/codegen/llvm_scope.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("Main function entry validation") {
    SECTION("Valid main entry with fn(args: [][:0]u8): void passes") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            pub const main := fn(args: [][:0]u8): void {
                return;
            };
        )")};
        CHECK(ctx->analyzer.validate_main_entry(ctx->root_mod));
    }

    SECTION("Missing main function returns error") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            pub const not_main := fn(args: [][:0]u8): void {
                return;
            };
        )")};
        CHECK_FALSE(ctx->analyzer.validate_main_entry(ctx->root_mod));
    }

    SECTION("Private main function (not pub) returns error") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const main := fn(args: [][:0]u8): void {
                return;
            };
        )")};
        CHECK_FALSE(ctx->analyzer.validate_main_entry(ctx->root_mod));
    }

    SECTION("Main with 0 params is valid") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            pub const main := fn(): void {
                return;
            };
        )")};
        CHECK(ctx->analyzer.validate_main_entry(ctx->root_mod));
    }

    SECTION("Main with 0 params and i32 return is valid") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            pub const main := fn(): i32 {
                return 0;
            };
        )")};
        CHECK(ctx->analyzer.validate_main_entry(ctx->root_mod));
    }

    SECTION("Main with non-void and non-i32 return returns error") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            pub const main := fn(args: [][:0]u8): bool {
                return true;
            };
        )")};
        CHECK_FALSE(ctx->analyzer.validate_main_entry(ctx->root_mod));
    }

    SECTION("Main with i32 return is valid") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            pub const main := fn(args: [][:0]u8): i32 {
                return 0;
            };
        )")};
        CHECK(ctx->analyzer.validate_main_entry(ctx->root_mod));
    }

    SECTION("Main with incorrect param type returns error") {
        constexpr auto input = R"(
            pub const main := fn(args: []u8): void {
                return;
            };
        )";
        auto [ctx, idx]{helpers::resolve_and_check(input)};
        CHECK_FALSE(ctx->analyzer.validate_main_entry(ctx->root_mod));
    }
}

TEST_CASE("Executable LLVM IR lowering with entry wrapper") {
    codegen::llvm_scope scope;
    llvm::LLVMContext   context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const main := fn(args: [][:0]u8): void {
            return;
        };
    )")};
    REQUIRE(ctx->analyzer.validate_main_entry(ctx->root_mod));
    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir_executable(*ctx, context))};

    // Verify module is valid LLVM IR
    std::string              err_str;
    llvm::raw_string_ostream os{err_str};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod, &os));

    // Verify user main was renamed to _ghoti_main
    auto& ghoti_main_fn{UNWRAP(llvm_mod->getFunction("_ghoti_main"))};
    CHECK(ghoti_main_fn.getReturnType()->isVoidTy());
    CHECK(ghoti_main_fn.arg_size() == 1);

    // Verify C entry wrapper main(i32, ptr) was generated
    auto& c_main_fn{UNWRAP(llvm_mod->getFunction("main"))};
    CHECK(c_main_fn.getReturnType()->isIntegerTy(32));
    CHECK(c_main_fn.arg_size() == 2);
    CHECK(c_main_fn.getArg(0)->getType()->isIntegerTy(32));
    CHECK(c_main_fn.getArg(1)->getType()->isPointerTy());
}

TEST_CASE("Parameterless main function lowering") {
    codegen::llvm_scope scope;
    llvm::LLVMContext   context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const main := fn(): i32 {
            return 42;
        };
    )")};
    REQUIRE(ctx->analyzer.validate_main_entry(ctx->root_mod));
    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir_executable(*ctx, context))};

    std::string              err_str;
    llvm::raw_string_ostream os{err_str};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod, &os));

    auto& ghoti_main_fn{UNWRAP(llvm_mod->getFunction("_ghoti_main"))};
    CHECK(ghoti_main_fn.getReturnType()->isIntegerTy(32));
    CHECK(ghoti_main_fn.arg_size() == 0);
}

TEST_CASE("@setMainSymbol sets user main correctly") {
    codegen::llvm_scope scope;
    llvm::LLVMContext   context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        @setMainSymbol("my_main");
        pub const my_main := fn(): i32 {
            return 42;
        };
    )")};
    CHECK(ctx->analyzer.get_ctx().user_main_name == "my_main");
    REQUIRE(ctx->analyzer.validate_main_entry(ctx->root_mod));
    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir_executable(*ctx, context))};

    std::string              err_str;
    llvm::raw_string_ostream os{err_str};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod, &os));

    auto& ghoti_main_fn{UNWRAP(llvm_mod->getFunction("_ghoti_main"))};
    CHECK(ghoti_main_fn.getReturnType()->isIntegerTy(32));
    CHECK(ghoti_main_fn.arg_size() == 0);
    CHECK(llvm_mod->getFunction("main"));
}

} // namespace ghoti::tests
