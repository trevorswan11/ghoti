#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "ghoti/config.h"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("@cfg selects by the target CPU's features") {
    constexpr std::string_view source{R"(
        @cfg (cpu.has(.neon)) {
            pub const with_neon = 1;
        }
        @cfg (arch == .x86_64 and cpu.has(.sse2)) {
            pub const with_sse2 = 1;
        }
        @cfg (cpu.has(.avx512f)) {
            pub const with_avx512 = 1;
        }
        @cfg (cpu.has(.m) and cpu.has(.c)) {
            pub const with_rv64gc = 1;
        }
    )"};
    const auto                 declares{[&](std::string_view triple, std::string_view name) {
        auto [ctx, idx]{helpers::resolve_for_target(source, triple)};
        return ctx->root_mod.ast.find_top_level_decl(name).has_value();
    }};

    CHECK(declares("x86_64-unknown-linux-gnu", "with_sse2"));
    CHECK_FALSE(declares("x86_64-unknown-linux-gnu", "with_neon"));
    CHECK_FALSE(declares("x86_64-unknown-linux-gnu", "with_avx512"));
    CHECK(declares("aarch64-unknown-linux-gnu", "with_neon"));
    CHECK_FALSE(declares("aarch64-unknown-linux-gnu", "with_sse2"));
    CHECK(declares("riscv64-unknown-linux-gnu", "with_rv64gc"));
    CHECK_FALSE(declares("aarch64-unknown-linux-gnu", "with_rv64gc"));
}

TEST_CASE("@cfg rejects a CPU feature no architecture has") {
    const auto out{helpers::run_cfg(R"(
        @cfg (cpu.has(.avx3)) { const a = 1; }
    )")};
    CHECK(out.has_code(sema::error::CFG_UNKNOWN_ENUM_MEMBER));
    CHECK(out.any_message_contains("no architecture has a CPU feature 'avx3'"));
    CHECK(out.any_message_contains("did you mean"));
}

TEST_CASE("@cfgValue reads a CPU feature") {
    auto [ctx, idx]{helpers::resolve_for_target(R"(
        pub const neon = @cfgValue(cpu.has(.neon));
        pub const check = fn(): i32 { return if (neon) 1 else 0; };
    )",
                                                "aarch64-unknown-linux-gnu")};
    CHECK(ctx->root_mod.ast.find_top_level_decl("check").has_value());
}

#if GHOTI_ASM_HOST_X86_64

TEST_CASE("builtin.cpu reports the target CPU, at compile time and at runtime") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut out: i32 = 0;
            if comptime (builtin.cpu.has(.sse2)) { out += 1; }
            if comptime (!builtin.cpu.has(.avx512f)) { out += 2; }
            if (builtin.cpu.has(.@"64bit")) { out += 4; }
            let c = builtin.cpu;
            if (c.features.has(.cx16)) { out += 8; }
            if (c.model.len == 7) { out += 16; } // "generic"
            return out;
        };
    )") == 31);
}

TEST_CASE("A CpuFeatureSet can be built and queried at runtime") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut set: builtin.CpuFeatureSet = .{};
            set.set(.avx2, true);
            set.set(.sse4_2, true);
            set.set(.avx2, false);
            let x86: builtin.x86.Feature = .avx512f;
            set.set(x86, true);
            return if (!set.has(.avx2) and set.has(.sse4_2) and set.has(.avx512f)) 42 else 0;
        };
    )") == 42);
}

#endif

} // namespace ghoti::tests
