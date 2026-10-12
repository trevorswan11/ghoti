#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/sema/error.hh"
#include "ghoti/config.h"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] auto intrinsic_error(std::string_view body,
                                   std::string_view triple = "x86_64-unknown-linux-gnu")
    -> helpers::resolve_result {
    return helpers::resolve_diags_for_target(
        fmt::format("pub const f = fn(x: u32, p: ^u8, flag: bool): void {{ {} }};", body), triple);
}

} // namespace

TEST_CASE("@llvmIntrinsic calls a target-independent intrinsic") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let x: u32 = 0xF0F0;
            let pop = @llvmIntrinsic("llvm.ctpop", u32, x);        // overloaded, by base name
            let pop32 = @llvmIntrinsic("llvm.ctpop.i32", u32, x);  // by mangled name
            let lz = @llvmIntrinsic("llvm.ctlz", u32, x, false);   // `bool` immediate
            let buf: [4]u8 = .{ 1, 2, 3, 4 };
            @llvmIntrinsic("llvm.prefetch", void, buf.ptr, 0, 3, 1); // untyped literals are `i32`
            let ticks = @llvmIntrinsic("llvm.readcyclecounter", u64);
            _ = ticks;
            return @intCast(i32, pop + pop32 + lz);
        };
    )") == 8 + 8 + 16);
}

#if GHOTI_ASM_HOST_X86_64

TEST_CASE("@llvmIntrinsic calls the target's own intrinsics") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            @llvmIntrinsic("llvm.x86.sse2.pause", void);
            return 42;
        };
    )") == 42);
}

#endif

TEST_CASE("@llvmIntrinsic checks the name, the target, and the signature") {
    SECTION("a name LLVM doesn't know") {
        const auto res{intrinsic_error(R"(_ = @llvmIntrinsic("llvm.nope", u32);)")};
        CHECK(res.codes.front() == sema::error::INVALID_LLVM_INTRINSIC);
        CHECK(res.message_contains("LLVM has no intrinsic named 'llvm.nope'"));
    }

    SECTION("another architecture's intrinsic") {
        CHECK(intrinsic_error(R"(@llvmIntrinsic("llvm.aarch64.isb", void, 0);)")
                  .message_contains("is an intrinsic for 'aarch64', but the target is 'x86_64'"));
        CHECK(intrinsic_error(R"(@llvmIntrinsic("llvm.aarch64.isb", void, 0);)",
                              "aarch64-unknown-linux-gnu")
                  .codes.empty());
    }

    SECTION("types that fit no overload") {
        CHECK(intrinsic_error(R"(_ = @llvmIntrinsic("llvm.ctpop", u32, flag);)")
                  .message_contains("'llvm.ctpop' has no overload"));
        CHECK(intrinsic_error(R"(_ = @llvmIntrinsic("llvm.ctpop", u32);)")
                  .message_contains("Expected 1, but got 0"));
        CHECK(intrinsic_error(R"(_ = @llvmIntrinsic("llvm.readcyclecounter", u32);)")
                  .message_contains("'llvm.readcyclecounter' is 'i64 ()', not 'i32 ()'"));
    }

    SECTION("a mangled name that disagrees with the types") {
        CHECK(intrinsic_error(R"(_ = @llvmIntrinsic("llvm.ctpop.i64", u32, x);)")
                  .message_contains("names a different overload than these types call: "
                                    "'llvm.ctpop.i32'"));
    }

    SECTION("an immediate that isn't known at compile time") {
        CHECK(intrinsic_error(R"(_ = @llvmIntrinsic("llvm.ctlz", u32, x, flag);)")
                  .message_contains("argument 2 of 'llvm.ctlz' must be known at compile time"));
    }

    SECTION("a name that isn't a compile-time string, and a missing return type") {
        CHECK(intrinsic_error(R"(_ = @llvmIntrinsic(x, u32);)")
                  .message_contains("needs a compile-time string naming the intrinsic"));
        CHECK(intrinsic_error(R"(_ = @llvmIntrinsic("llvm.ctpop");)").codes.front() ==
              sema::error::ARITY_MISMATCH);
    }
}

} // namespace ghoti::tests
