#include <catch2/catch_test_macros.hpp>
#include <fmt/base.h>
#include <fmt/format.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>

#include "compiler/sema/analyzer.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("E2E LLVM Emission: Arithmetic, Loops and Multi-Function Calls") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const add := fn(a: i64, b: i64): i64 {
            return a + b;
        };

        pub const compute := fn(n: i64): i64 {
            var sum: i64 = 0l;
            var i: i64 = 1l;
            while (i <= n) {
                sum = add(sum, i);
                i = i + 1l;
            };
            return sum;
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));

    auto& fn_add{UNWRAP(llvm_mod->getFunction("add"))};
    CHECK(fn_add.getReturnType()->isIntegerTy(64));
    CHECK(fn_add.arg_size() == 2);

    auto& fn_comp{UNWRAP(llvm_mod->getFunction("compute"))};
    CHECK(fn_comp.getReturnType()->isIntegerTy(64));
    CHECK(fn_comp.arg_size() == 1);
}

TEST_CASE("E2E LLVM Emission: Struct Operations and Nested Aggregates") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Vector2 := struct {
            x: f64,
            y: f64,
        };

        pub const add_vectors := fn(v1: Vector2, v2: Vector2): Vector2 {
            return Vector2{
                .x = v1.x + v2.x,
                .y = v1.y + v2.y,
            };
        };

        pub const dot_product := fn(v1: Vector2, v2: Vector2): f64 {
            return (v1.x * v2.x) + (v1.y * v2.y);
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));
    CHECK(llvm_mod->getFunction("add_vectors"));
    CHECK(llvm_mod->getFunction("dot_product"));
}

TEST_CASE("E2E LLVM Emission: Array Manipulation, Mutation & Pointers") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const swap := fn(a: ^mut i32, b: ^mut i32): void {
            const tmp: i32 = *a;
            *a = *b;
            *b = tmp;
        };

        pub const sum_array := fn(): i32 {
            var arr: [4uz]i32 = [4uz]i32{10, 20, 30, 40};
            var sum: i32 = 0;
            var i: usize = 0uz;
            while (i < 4uz) {
                sum = sum + arr[i];
                i = i + 1uz;
            };
            return sum;
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));
    CHECK(llvm_mod->getFunction("swap"));
    CHECK(llvm_mod->getFunction("sum_array"));
}

} // namespace ghoti::tests
