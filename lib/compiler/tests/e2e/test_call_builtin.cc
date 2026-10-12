#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

// The calls `caller` makes to `callee`, in order
[[nodiscard]] auto calls_to(const llvm::Module& mod,
                            std::string_view    caller,
                            std::string_view    callee) -> std::vector<const llvm::CallInst*> {
    std::vector<const llvm::CallInst*> out;
    const auto*                        fn{mod.getFunction(caller)};
    if (!fn) { return out; }
    for (const auto& block : *fn) {
        for (const auto& inst : block) {
            const auto* call{llvm::dyn_cast<llvm::CallInst>(&inst)};
            if (call && call->getCalledFunction() &&
                call->getCalledFunction()->getName() == llvm::StringRef{callee}) {
                out.emplace_back(call);
            }
        }
    }
    return out;
}

} // namespace

TEST_CASE("@call makes the call its modifier asks for") {
    CHECK(helpers::compile_and_run(R"(
        const add = fn(a: i32, b: i32): i32 { return a + b; };
        const square = fn(x: i32): i32 { return x * x; };
        const sum = fn(xs...): i32 { return @call(.never_inline, add, xs...); };
        const count = fn(n: i32, acc: i32): i32 {
            if (n == 0) { return acc; }
            return @call(.always_tail, count, n - 1, acc + 1);
        };
        pub const main = fn(): i32 {
            let mut out: i32 = @call(.auto, add, 1, 2);       // 3
            out += @call(.never_inline, add, 3, 4);           // 7
            out += @call(.always_inline, add, 5, 6);          // 11
            out += @call(.never_tail, add, 7, 8);             // 15
            const folded = @call(.compile_time, square, 4);   // 16
            out += folded + sum(1, 2);                        // 19
            out += count(1000, 0) - 1000;                     // 0
            return out;
        };
    )") == 3 + 7 + 11 + 15 + 19);
}

TEST_CASE("Codegen: @call marks the LLVM call") {
    llvm::LLVMContext context;
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const add = fn(a: i32, b: i32): i32 { return a + b; };
        pub const no_tail = fn(): i32 { return @call(.never_tail, add, 1, 2); };
        pub const no_inline = fn(): i32 { return @call(.never_inline, add, 1, 2); };
        pub const inlined = fn(): i32 { return @call(.always_inline, add, 1, 2); };
        pub const tail = fn(a: i32, b: i32): i32 { return @call(.always_tail, add, a, b); };
        pub const main = fn(): void {};
    )")};
    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};

    const auto only{[&](std::string_view caller) -> const llvm::CallInst& {
        const auto calls{calls_to(*llvm_mod, caller, "add")};
        REQUIRE(calls.size() == 1);
        return *calls.front();
    }};
    CHECK(only("no_tail").getTailCallKind() == llvm::CallInst::TCK_NoTail);
    CHECK(only("no_inline").hasFnAttr(llvm::Attribute::NoInline));
    // Already inlined: the call is gone from its caller
    REQUIRE(llvm_mod->getFunction("inlined"));
    CHECK(calls_to(*llvm_mod, "inlined", "add").empty());
    CHECK(only("tail").getTailCallKind() == llvm::CallInst::TCK_MustTail);
}

TEST_CASE("@call rejects a modifier it can't honor") {
    const auto error_of{[](std::string_view source) {
        auto [ctx, idx]{helpers::expect_compile_error(source)};
        const auto diags{ctx->root_mod.diagnostics.as_opt<sema::diagnostics>()};
        REQUIRE(diags);
        return std::string{(*diags)[0].get_message().value_or("")};
    }};

    SECTION("an inline modifier against the function's own attribute") {
        CHECK(error_of(R"(
            const f = @[inline(.never)] fn(x: i32): i32 { return x; };
            pub const main = fn(): i32 { return @call(.always_inline, f, 1); };
        )")
                  .contains("the function is `@[inline(.never)]`"));
        CHECK(error_of(R"(
            const f = @[inline(.always)] fn(x: i32): i32 { return x; };
            pub const main = fn(): i32 { return @call(.never_inline, f, 1); };
        )")
                  .contains("the function is `@[inline(.always)]`"));
    }

    SECTION("a tail call that isn't returned directly") {
        CHECK(error_of(R"(
            let mut counter: i32 = 0;
            const f = fn(x: i32): i32 { return x; };
            const g = fn(x: i32): i32 {
                defer { counter += 1; }
                return @call(.always_tail, f, x);
            };
            pub const main = fn(): i32 { return g(1); };
        )")
                  .contains("the call must be returned directly"));
    }

    SECTION("a tail call to a different prototype") {
        CHECK(error_of(R"(
            const f = fn(x: i32): i64 { return @intCast(i64, x); };
            const g = fn(x: i32): i32 { return @call(.always_tail, f, x); };
            pub const main = fn(): i32 { return g(1); };
        )")
                  .contains("needs the same parameters, result, and calling convention"));
    }

    SECTION("a compile-time call of runtime values") {
        CHECK(error_of(R"(
            const f = fn(x: i32): i32 { return x; };
            pub const main = fn(): i32 {
                let mut r: i32 = 0;
                r = @call(.compile_time, f, r);
                return r;
            };
        )")
                  .contains("can't be evaluated at compile time"));
    }

    SECTION("a builtin, an unknown modifier, and a malformed call") {
        CHECK(helpers::resolve_diags("const f = fn(): f64 { return @call(.auto, @sqrt, 1.0); };")
                  .message_contains("a builtin isn't one"));
        CHECK(helpers::resolve_diags("const add = fn(a: i32): i32 { return a; }; "
                                     "const f = fn(): i32 { return @call(.sometimes, add, 1); };")
                  .message_contains("'CallModifier' has no field named 'sometimes'"));
    }
}

} // namespace ghoti::tests
