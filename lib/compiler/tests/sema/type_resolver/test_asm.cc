#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/option.hh>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

// Resolves `body` inside a trivial function and returns the first sema diagnostic's code
[[nodiscard]] auto resolve_asm_error(std::string_view body) -> stdx::option<sema::error> {
    const auto src{fmt::format("pub const f := fn(n: i64, fd: i64): void {{ {} }};", body)};
    auto [ctx, idx]{helpers::resolve(src)};

    // Resolver diagnostics are moved out of the shared context and into the module.
    const auto diags{ctx->root_mod.diagnostics.as_opt<sema::diagnostics>()};
    if (!diags || diags->empty()) { return stdx::none; }
    return (*diags)[0].get_error();
}

} // namespace

TEST_CASE("A no-output asm must be volatile") {
    CHECK(resolve_asm_error(R"(asm { template: "syscall", inputs: ("{rax}" = n) };)") ==
          sema::error::ILLEGAL_INLINE_ASM);
    CHECK_FALSE(resolve_asm_error(
        R"(asm { template: "syscall", inputs: ("{rax}" = n), options: (volatile) };)"));
}

TEST_CASE("A `_` result slot requires an explicit result type") {
    CHECK(resolve_asm_error(
              R"(asm { template: "rdtsc", outputs: ("={eax}" = _), options: (volatile) };)") ==
          sema::error::ILLEGAL_INLINE_ASM);
}

TEST_CASE("A result type without a `_` slot is rejected") {
    CHECK(resolve_asm_error(R"(asm u32 { template: "nop", options: (volatile) };)") ==
          sema::error::ILLEGAL_INLINE_ASM);
}

TEST_CASE("Intel and att options conflict") {
    CHECK(resolve_asm_error(R"(asm { template: "nop", options: (volatile, intel, att) };)") ==
          sema::error::ILLEGAL_INLINE_ASM);
}

TEST_CASE("Noreturn asm cannot bind an output") {
    CHECK(
        resolve_asm_error(
            R"(asm u32 { template: "rdtsc", outputs: ("={eax}" = _), options: (volatile, noreturn) };)") ==
        sema::error::ILLEGAL_INLINE_ASM);
}

TEST_CASE("Template placeholder must reference a real operand") {
    CHECK(
        resolve_asm_error(
            R"(asm { template: "mov %0, %2", inputs: ("r" = n, "r" = fd), options: (volatile) };)") ==
        sema::error::ILLEGAL_INLINE_ASM);
    CHECK_FALSE(resolve_asm_error(
        R"(asm { template: "mov %0, %1", inputs: ("r" = n, "r" = fd), options: (volatile) };)"));
}

TEST_CASE("An asm output operand must be an lvalue") {
    CHECK(resolve_asm_error(
              R"(asm { template: "nop", outputs: ("=r" = n + fd), options: (volatile) };)") ==
          sema::error::ILLEGAL_INLINE_ASM);
}

} // namespace ghoti::tests
