#include <string>

#include <catch2/catch_test_macros.hpp>

#include "helpers/gir.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("GIR: inline asm lowers to an INLINE_ASM instruction") {
    auto       ctx_idx{helpers::resolve_and_check(R"(
        pub const sys_write := fn(n: i64, fd: i64, buf: ^u8, len: usize): void {
            asm {
                template: "syscall",
                inputs: ("{rax}" = n, "{rdi}" = fd, "{rsi}" = buf, "{rdx}" = len),
                clobbers: ("rcx", "r11", "memory"),
                options: (volatile),
            };
        };
    )")};
    const auto dump{helpers::dump_gir(ctx_idx)};

    CHECK(dump.contains("inline_asm volatile \"syscall\""));
    CHECK(dump.contains("{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}"));
}

TEST_CASE("GIR: inline asm result slot yields a typed temporary") {
    auto       ctx_idx{helpers::resolve_and_check(R"(
        pub const timestamp := fn(): u32 {
            const lo := asm u32 {
                template: "rdtsc",
                outputs: ("={eax}" = _),
                options: (volatile),
            };
            return lo;
        };
    )")};
    const auto dump{helpers::dump_gir(ctx_idx)};

    CHECK(dump.contains("= inline_asm volatile \"rdtsc\" \"={eax}\""));
}

TEST_CASE("GIR: noreturn inline asm is followed by unreachable") {
    auto       ctx_idx{helpers::resolve_and_check(R"(
        pub const halt := fn(code: i64): void {
            asm {
                template: "syscall",
                inputs: ("{rax}" = code),
                options: (volatile, noreturn),
            };
        };
    )")};
    const auto dump{helpers::dump_gir(ctx_idx)};

    CHECK(dump.contains("inline_asm volatile noreturn \"syscall\""));
    CHECK(dump.contains("unreachable"));
}

} // namespace ghoti::tests
