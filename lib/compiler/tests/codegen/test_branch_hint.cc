#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ProfDataUtils.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

// The first conditional branch in `fn` that carries metadata of `kind`
[[nodiscard]] auto hinted_branch(const llvm::Function& fn, unsigned kind)
    -> stdx::option<const llvm::BranchInst&> {
    for (const auto& bb : fn) {
        if (const auto* br{llvm::dyn_cast<llvm::BranchInst>(bb.getTerminator())};
            br && br->isConditional() && br->getMetadata(kind)) {
            return *br;
        }
    }
    return stdx::none;
}

[[nodiscard]] auto weights_of(const llvm::BranchInst& br) -> llvm::SmallVector<u32, 2> {
    llvm::SmallVector<u32, 2> weights;
    CHECK(llvm::extractBranchWeights(br, weights));
    return weights;
}

} // namespace

TEST_CASE("Codegen: @branchHint weights the branch it opens") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const likely_then := fn(c: bool): i32 {
            if (c) {
                @branchHint(.likely);
                return 1;
            } else {
                return 2;
            }
        };
        pub const cold_else := fn(c: bool): i32 {
            if (c) {
                return 1;
            } else {
                @branchHint(.cold);
                return 2;
            }
        };
        pub const coin := fn(c: bool): i32 {
            if (c) {
                @branchHint(.unpredictable);
                return 1;
            }
            return 2;
        };
        pub const classify := fn(x: i32): i32 {
            let mut out: i32 = 20;
            match (x) {
                0 => {
                    @branchHint(.unlikely);
                    out = 10;
                },
                _ => {},
            };
            return out;
        };
        pub const main := fn(args: [][:0]u8): i32 {
            return likely_then(true) + cold_else(false) + coin(true) + classify(3);
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));

    const auto likely{weights_of(UNWRAP(
        hinted_branch(UNWRAP(llvm_mod->getFunction("likely_then")), llvm::LLVMContext::MD_prof)))};
    CHECK(likely[0] > likely[1]);

    const auto cold{weights_of(UNWRAP(
        hinted_branch(UNWRAP(llvm_mod->getFunction("cold_else")), llvm::LLVMContext::MD_prof)))};
    CHECK(cold[0] > cold[1]);

    CHECK(
        hinted_branch(UNWRAP(llvm_mod->getFunction("coin")), llvm::LLVMContext::MD_unpredictable));

    const auto arm{weights_of(UNWRAP(
        hinted_branch(UNWRAP(llvm_mod->getFunction("classify")), llvm::LLVMContext::MD_prof)))};
    CHECK(arm[0] < arm[1]);
}

TEST_CASE("Codegen: @branchHint(.cold) opening a function body marks it cold") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const slow := fn(): i32 {
            @branchHint(.cold);
            return 3;
        };
        pub const fast := fn(): i32 { return 4; };
        pub const main := fn(args: [][:0]u8): i32 { return slow() + fast(); };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK(UNWRAP(llvm_mod->getFunction("slow")).hasFnAttribute(llvm::Attribute::Cold));
    CHECK_FALSE(UNWRAP(llvm_mod->getFunction("fast")).hasFnAttribute(llvm::Attribute::Cold));
}

TEST_CASE("@branchHint must open a branch, arm, or function body") {
    SECTION("not the first statement") {
        CHECK(helpers::raised(R"(
            const f := fn(c: bool): i32 {
                if (c) {
                    let x := 1;
                    @branchHint(.likely);
                    return x;
                }
                return 0;
            };
        )",
                              sema::error::TYPE_MISMATCH));
    }
    SECTION("in a plain nested block") {
        CHECK(helpers::raised(R"(
            const f := fn(): i32 {
                {
                    @branchHint(.likely);
                }
                return 0;
            };
        )",
                              sema::error::TYPE_MISMATCH));
    }
    SECTION("a function body accepts only .cold") {
        CHECK(helpers::raised("const f := fn(): i32 { @branchHint(.likely); return 0; };",
                              sema::error::TYPE_MISMATCH));
    }
    SECTION("the hint must be known at compile time") {
        CHECK(helpers::raised(R"(
            const f := fn(c: bool, h: builtin.BranchHint): i32 {
                if (c) {
                    @branchHint(h);
                    return 1;
                }
                return 0;
            };
        )",
                              sema::error::TYPE_MISMATCH));
    }
}

} // namespace ghoti::tests
