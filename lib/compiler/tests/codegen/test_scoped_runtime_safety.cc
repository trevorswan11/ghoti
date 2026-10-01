#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

// Whether `fn` carries an overflow-checked add, i.e. runtime safety was on for its body
[[nodiscard]] auto has_checked_add(const llvm::Function& fn) -> bool {
    for (const auto& bb : fn) {
        for (const auto& inst : bb) {
            if (const auto* call{llvm::dyn_cast<llvm::CallBase>(&inst)}) {
                const auto* callee{call->getCalledFunction()};
                if (callee && callee->getName().contains("add.with.overflow")) { return true; }
            }
        }
    }
    return false;
}

} // namespace

TEST_CASE("Codegen: @setRuntimeSafety scopes checks to the rest of its block") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const checked := fn(a: i32, b: i32): i32 { return a + b; };
        pub const unchecked := fn(a: i32, b: i32): i32 {
            @setRuntimeSafety(false);
            return a + b;
        };
        pub const inner_only := fn(a: i32, b: i32): i32 {
            {
                @setRuntimeSafety(false);
            }
            return a + b;
        };
        pub const reenabled := fn(a: i32, b: i32): i32 {
            @setRuntimeSafety(false);
            let mut sum: i32 = 0;
            {
                @setRuntimeSafety(true);
                sum = a + b;
            }
            return sum;
        };
        pub const calls_checked := fn(a: i32, b: i32): i32 {
            @setRuntimeSafety(false);
            return checked(a, b);
        };
        pub const main := fn(args: [][:0]u8): i32 {
            return checked(1, 2) + unchecked(1, 2) + inner_only(1, 2) + reenabled(1, 2) +
                   calls_checked(1, 2);
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));

    CHECK(has_checked_add(UNWRAP(llvm_mod->getFunction("checked"))));
    CHECK_FALSE(has_checked_add(UNWRAP(llvm_mod->getFunction("unchecked"))));
    CHECK(has_checked_add(UNWRAP(llvm_mod->getFunction("inner_only"))));
    CHECK(has_checked_add(UNWRAP(llvm_mod->getFunction("reenabled"))));
}

TEST_CASE("@runtimeSafety() observes the enclosing @setRuntimeSafety") {
    helpers::resolve_and_check(R"(
        const f := fn(): i32 {
            comptime { @assert(@runtimeSafety()); }
            @setRuntimeSafety(false);
            comptime { @assert(!@runtimeSafety()); }
            const nested := fn(): i32 {
                comptime { @assert(@runtimeSafety()); }
                return 0;
            };
            return nested();
        };
        pub const main := fn(): i32 { return f(); };
    )");
}

TEST_CASE("@setRuntimeSafety is validated") {
    CHECK(helpers::raised(R"(
        const f := fn(on: bool): void { @setRuntimeSafety(on); };
    )",
                          sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised("const x := @setRuntimeSafety(false);", sema::error::TYPE_MISMATCH));
}

} // namespace ghoti::tests
