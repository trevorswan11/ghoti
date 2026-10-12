#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("@[target_features] adds CPU features to one function") {
    llvm::LLVMContext context;
    const auto        features_of{
        [&](std::string_view source, std::string_view triple, std::string_view fn) -> std::string {
            auto [ctx, idx]{helpers::resolve_for_target(source, triple)};
            auto        llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
            const auto* llvm_fn{llvm_mod->getFunction(fn)};
            REQUIRE(llvm_fn);
            if (!llvm_fn->hasFnAttribute("target-features")) { return ""; }
            return llvm_fn->getFnAttribute("target-features").getValueAsString().str();
        }};

    constexpr std::string_view x86{R"(
        pub const wide = @[target_features(.avx2, .sse4_2)] fn(x: i32): i32 { return x; };
        pub const plain = fn(x: i32): i32 { return x; };
    )"};
    // LLVM spells `sse4_2` with a dot
    CHECK(features_of(x86, "x86_64-unknown-linux-gnu", "wide").contains("+avx2,+sse4.2"));
    CHECK(features_of(x86, "x86_64-unknown-linux-gnu", "plain").empty());
    CHECK(features_of(R"(
        pub const dot = @[target_features(.dotprod)] fn(x: i32): i32 { return x; };
    )",
                      "aarch64-unknown-linux-gnu",
                      "dot")
              .contains("+dotprod"));
}

TEST_CASE("@[target_features] takes only the target's CPU features") {
    CHECK(helpers::resolve_diags(R"(
        const f = @[target_features(1)] fn(): void {};
    )")
              .message_contains("requires compile-time 'builtin.CpuFeature' values"));
    // Another architecture's feature isn't one of the target's
    const auto neon{helpers::resolve_diags_for_target(R"(
        const f = @[target_features(.avx2)] fn(): void {};
    )",
                                                      "aarch64-unknown-linux-gnu")};
    CHECK(neon.message_contains("has no field named 'avx2'"));
    CHECK(neon.messages.size() == 1);
}

} // namespace ghoti::tests
