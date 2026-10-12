#include <algorithm>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/target.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] auto enabled(const codegen::target_options& opts) -> std::vector<std::string> {
    return UNWRAP(codegen::resolve_target_cpu(opts)).enabled;
}

[[nodiscard]] auto has(const std::vector<std::string>& features, std::string_view name) -> bool {
    return std::ranges::contains(features, name);
}

} // namespace

TEST_CASE("CPU feature families come from LLVM's tables, named as ghoti spells them") {
    const auto& families{codegen::cpu_feature_families()};
    for (const std::string_view name : {"x86", "aarch64", "arm", "riscv", "wasm"}) {
        INFO(name);
        CHECK(std::ranges::any_of(families, [&](const auto& f) { return f.name == name; }));
    }

    const auto& x86{*std::ranges::find(families, "x86", &codegen::cpu_feature_family::name)};
    CHECK(x86.llvm_names.size() == x86.ghoti_names.size());
    CHECK(has(x86.ghoti_names, "avx2"));
    CHECK(has(x86.ghoti_names, "sse4_2"));
    CHECK_FALSE(has(x86.ghoti_names, "sse4.2"));
    CHECK(has(x86.llvm_names, "sse4.2"));
    CHECK(has(x86.ghoti_names, "64bit"));
}

TEST_CASE("The target CPU's features follow --cpu, --features, and ghoti's baselines") {
    SECTION("generic x86-64 is SSE2 plus cmpxchg16b, never AVX") {
        const auto features{enabled({.triple_str = "x86_64-unknown-linux-gnu"})};
        CHECK(has(features, "sse2"));
        CHECK(has(features, "cx16"));
        CHECK_FALSE(has(features, "avx2"));
    }

    SECTION("a named model brings its features") {
        const auto features{enabled({.triple_str = "x86_64-unknown-linux-gnu", .cpu = "haswell"})};
        CHECK(has(features, "avx2"));
        CHECK(has(features, "fma"));
        CHECK_FALSE(has(features, "avx512f"));
        CHECK(UNWRAP(codegen::resolve_target_cpu(
                         {.triple_str = "x86_64-unknown-linux-gnu", .cpu = "haswell"}))
                  .model == "haswell");
    }

    SECTION("--features adds and removes on top of the model") {
        const auto features{enabled({.triple_str = "x86_64-unknown-linux-gnu",
                                     .cpu        = "haswell",
                                     .features   = "+avx512f,-bmi2"})};
        CHECK(has(features, "avx512f"));
        CHECK_FALSE(has(features, "bmi2"));
    }

    SECTION("RISC-V Linux is RV64GC") {
        const auto features{enabled({.triple_str = "riscv64-unknown-linux-gnu"})};
        for (const std::string_view f : {"m", "a", "f", "d", "c"}) {
            INFO(f);
            CHECK(has(features, f));
        }
    }

    SECTION("AArch64 always has NEON") {
        CHECK(has(enabled({.triple_str = "aarch64-unknown-linux-gnu"}), "neon"));
    }

    SECTION("native describes this machine") {
        const auto cpu{UNWRAP(codegen::resolve_target_cpu({.cpu = "native"}))};
        CHECK_FALSE(cpu.model.empty());
        CHECK(cpu.model != "native");
        CHECK(cpu.family.has_value());
    }
}

TEST_CASE("An unknown CPU or feature is an error, not LLVM's warning") {
    const auto error_of{[](const codegen::target_options& opts) {
        const auto cpu{codegen::resolve_target_cpu(opts)};
        REQUIRE_FALSE(cpu);
        CHECK(cpu.error().get_error() == codegen::error::UNKNOWN_CPU);
        return cpu.error().get_message().value_or("");
    }};

    CHECK(error_of({.triple_str = "x86_64-unknown-linux-gnu", .cpu = "skylak"})
              .contains("Unknown CPU 'skylak'"));
    CHECK(error_of({.triple_str = "x86_64-unknown-linux-gnu", .features = "+avx9"})
              .contains("Unknown CPU feature 'avx9'"));
    // Another arch's feature is no feature of this one
    CHECK(error_of({.triple_str = "x86_64-unknown-linux-gnu", .features = "+neon"})
              .contains("Unknown CPU feature 'neon'"));
    // `native` can only describe the machine's own architecture
    const auto foreign{codegen::resolve_target_triple().isX86() ? "aarch64-unknown-linux-gnu"
                                                                : "x86_64-unknown-linux-gnu"};
    CHECK(error_of({.triple_str = foreign, .cpu = "native"}).contains("'--cpu native'"));

    // `create_target_machine` rejects them the same way
    CHECK_FALSE(codegen::create_target_machine(
        {.triple_str = "x86_64-unknown-linux-gnu", .cpu = "skylak"}));
}

} // namespace ghoti::tests
