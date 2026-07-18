#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/ManagedStatic.h>
#include <magic_enum/magic_enum.hpp>
#include <stdx/enum.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/codegen/opt_level.hh"
#include "compiler/sema/analyzer.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("Optimizer over non trivial function at all levels") {
    helpers::llvm_test_scope scope;
    llvm::LLVMContext        context;

    for (const auto level : stdx::enum_range<codegen::opt_level>()) {
        DYNAMIC_SECTION("Level " << magic_enum::enum_name(level)) {
            auto [ctx, idx]{helpers::resolve_and_check(R"(
                pub const calc := fn(a: i64, b: i64): i64 {
                    var sum: i64 = a;
                    var i: i64 = 0l;
                    while (i < b) {
                        sum = sum + i;
                        i = i + 1l;
                    };
                    return sum;
                };
            )")};

            codegen::optimizer_options options{
                .level       = level,
                .verify_each = true,
            };

            auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context, options))};
            CHECK_FALSE(llvm::verifyModule(*llvm_mod));
            auto& fn{UNWRAP(llvm_mod->getFunction("calc"))};
            CHECK_FALSE(fn.empty());
        }
    }
}

TEST_CASE("Optimizing, folding, and propagating constants") {
    helpers::llvm_test_scope scope;
    llvm::LLVMContext        context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const fold_me := fn(x: i64): i64 {
            var a: i64 = x * 0l;
            var b: i64 = a + 42l;
            return b;
        };
    )")};

    SECTION("O0 preserves instructions") {
        codegen::optimizer_options options{.level = codegen::opt_level::O0};
        auto                       llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context, options))};
        CHECK_FALSE(llvm::verifyModule(*llvm_mod));

        // At O0, there should be multiple instructions (alloca, stores, loads, binary ops)
        auto& fn{UNWRAP(llvm_mod->getFunction("fold_me"))};
        usize inst_count{0};
        for (const auto& bb : fn) { inst_count += bb.size(); }
        CHECK(inst_count > 1);
    }

    SECTION("O2 folds to constant 42") {
        codegen::optimizer_options options{.level = codegen::opt_level::O2};
        auto                       llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context, options))};
        CHECK_FALSE(llvm::verifyModule(*llvm_mod));

        auto& fn{UNWRAP(llvm_mod->getFunction("fold_me"))};
        REQUIRE_FALSE(fn.empty());

        auto& entry_bb{fn.getEntryBlock()};
        auto& ret_inst{UNWRAP(llvm::dyn_cast<llvm::ReturnInst>(entry_bb.getTerminator()))};
        auto& ret_val{UNWRAP(llvm::dyn_cast<llvm::ConstantInt>(ret_inst.getReturnValue()))};
        CHECK(ret_val.getSExtValue() == 42);
    }
}

TEST_CASE("Mem2Reg and Alloca elimination") {
    helpers::llvm_test_scope scope;
    llvm::LLVMContext        context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const add_vars := fn(a: i64, b: i64): i64 {
            var x: i64 = a;
            var y: i64 = b;
            return x + y;
        };
    )")};

    SECTION("O0 has allocas") {
        codegen::optimizer_options options{.level = codegen::opt_level::O0};
        auto                       llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context, options))};
        auto&                      fn{UNWRAP(llvm_mod->getFunction("add_vars"))};

        usize alloca_count{0};
        for (const auto& bb : fn) {
            for (const auto& inst : bb) {
                if (llvm::isa<llvm::AllocaInst>(&inst)) { ++alloca_count; }
            }
        }
        CHECK(alloca_count > 0);
    }

    SECTION("O2 eliminates all allocas") {
        codegen::optimizer_options options{.level = codegen::opt_level::O2};
        auto                       llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context, options))};
        auto&                      fn{UNWRAP(llvm_mod->getFunction("add_vars"))};

        usize alloca_count{0};
        for (const auto& bb : fn) {
            for (const auto& inst : bb) {
                if (llvm::isa<llvm::AllocaInst>(&inst)) { ++alloca_count; }
            }
        }
        CHECK(alloca_count == 0);
    }
}

TEST_CASE("Dead code elimination") {
    helpers::llvm_test_scope scope;
    llvm::LLVMContext        context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const dead_calc := fn(a: i64): i64 {
            var unused: i64 = a * 100l + 42l;
            return a;
        };
    )")};

    codegen::optimizer_options options{.level = codegen::opt_level::O2};
    auto                       llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context, options))};
    auto&                      fn{UNWRAP(llvm_mod->getFunction("dead_calc"))};

    // In O2, unused computation is eliminated and function simply returns parameter 'a'
    auto& entry_bb{fn.getEntryBlock()};
    auto& ret_inst{UNWRAP(llvm::dyn_cast<llvm::ReturnInst>(entry_bb.getTerminator()))};
    CHECK(ret_inst.getReturnValue() == fn.getArg(0));
}

TEST_CASE("Function inlining") {
    helpers::llvm_test_scope scope;
    llvm::LLVMContext        context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const helper := fn(x: i64): i64 {
            return x * 2l;
        };

        pub const caller := fn(a: i64): i64 {
            return helper(a);
        };
    )")};

    SECTION("O0 contains call to helper") {
        codegen::optimizer_options options{.level = codegen::opt_level::O0};
        auto                       llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context, options))};
        auto&                      fn{UNWRAP(llvm_mod->getFunction("caller"))};

        bool has_call{false};
        for (const auto& bb : fn) {
            for (const auto& inst : bb) {
                if (llvm::isa<llvm::CallInst>(&inst)) { has_call = true; }
            }
        }
        CHECK(has_call);
    }

    SECTION("O2 inlines helper into caller") {
        codegen::optimizer_options options{.level = codegen::opt_level::O2};
        auto                       llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context, options))};
        auto&                      fn{UNWRAP(llvm_mod->getFunction("caller"))};

        bool has_call{false};
        for (const auto& bb : fn) {
            for (const auto& inst : bb) {
                if (llvm::isa<llvm::CallInst>(&inst)) { has_call = true; }
            }
        }
        CHECK_FALSE(has_call);
    }
}

TEST_CASE("Optimizer with debug and timing flags") {
    helpers::llvm_test_scope scope;
    llvm::LLVMContext        context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const test_fn := fn(x: i32): i32 {
            return x + 1;
        };
    )")};

    codegen::optimizer_options options{
        .level         = codegen::opt_level::O1,
        .verify_each   = true,
        .debug_logging = true,
        .time_passes   = true,
    };

    auto result{UNWRAP(helpers::emit_llvm_ir(*ctx, context, options))};
    CHECK_FALSE(llvm::verifyModule(*result));
}

TEST_CASE("Optimization level parsing") {
    using codegen::opt_level;

    CHECK(codegen::parse_opt_level("0") == opt_level::O0);
    CHECK(codegen::parse_opt_level("O0") == opt_level::O0);
    CHECK(codegen::parse_opt_level("-O0") == opt_level::O0);

    CHECK(codegen::parse_opt_level("1") == opt_level::O1);
    CHECK(codegen::parse_opt_level("O1") == opt_level::O1);

    CHECK(codegen::parse_opt_level("2") == opt_level::O2);
    CHECK(codegen::parse_opt_level("O2") == opt_level::O2);

    CHECK(codegen::parse_opt_level("3") == opt_level::O3);
    CHECK(codegen::parse_opt_level("O3") == opt_level::O3);

    CHECK(codegen::parse_opt_level("s") == opt_level::Os);
    CHECK(codegen::parse_opt_level("Os") == opt_level::Os);

    CHECK(codegen::parse_opt_level("z") == opt_level::Oz);
    CHECK(codegen::parse_opt_level("Oz") == opt_level::Oz);

    CHECK_FALSE(codegen::parse_opt_level("invalid"));
}

} // namespace ghoti::tests
