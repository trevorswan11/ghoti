#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/sema/error.hh"
#include "ghoti/config.h"
#include "helpers/common.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("A non-C-ABI integer width is rejected in an extern struct field") {
    helpers::test_resolver_fail(
        "const S = extern struct { a: u100 };",
        sema::diagnostic{"extern struct field 'a' has type 'u100', which has no C ABI "
                         "representation; extern signatures accept 8/16/32/64-bit integers, "
                         "usize/isize, bool, f32, f64, and f80 on x86",
                         sema::error::ILLEGAL_REFERENCE_FIELD,
                         std::pair{0UZ, 29UZ}});
}

TEST_CASE("A non-C-ABI integer width is rejected in an extern union field") {
    helpers::test_resolver_fail(
        "const U = extern union { b: u7 };",
        sema::diagnostic{"extern union field 'b' has type 'u7', which has no C ABI "
                         "representation; extern signatures accept 8/16/32/64-bit integers, "
                         "usize/isize, bool, f32, f64, and f80 on x86",
                         sema::error::ILLEGAL_REFERENCE_FIELD,
                         std::pair{0UZ, 28UZ}});
}

TEST_CASE("A non-C-ABI parameter width is rejected in an extern fn signature") {
    helpers::test_resolver_fail(
        "extern const foo: fn(x: u3): void;",
        sema::diagnostic{"'fn(u3): void' has no C ABI representation; extern signatures accept "
                         "8/16/32/64-bit integers, usize/isize, bool, f32, f64, and f80 on x86",
                         sema::error::ILLEGAL_REFERENCE_FIELD,
                         std::pair{0UZ, 18UZ}});
}

TEST_CASE("A non-C-ABI return width is rejected in an extern fn signature") {
    helpers::test_resolver_fail(
        "extern const bar: fn(): i17;",
        sema::diagnostic{"'fn(): i17' has no C ABI representation; extern signatures accept "
                         "8/16/32/64-bit integers, usize/isize, bool, f32, f64, and f80 on x86",
                         sema::error::ILLEGAL_REFERENCE_FIELD,
                         std::pair{0UZ, 18UZ}});
}

TEST_CASE("f16 is rejected on an extern global") {
    helpers::test_resolver_fail(
        "extern let mut g: f16;",
        sema::diagnostic{"'f16' has no C ABI representation; extern signatures accept "
                         "8/16/32/64-bit integers, usize/isize, bool, f32, f64, and f80 on x86",
                         sema::error::ILLEGAL_REFERENCE_FIELD,
                         std::pair{0UZ, 18UZ}});
}

TEST_CASE("f128 is rejected on an extern global") {
    helpers::test_resolver_fail(
        "extern let mut g: f128;",
        sema::diagnostic{"'f128' has no C ABI representation; extern signatures accept "
                         "8/16/32/64-bit integers, usize/isize, bool, f32, f64, and f80 on x86",
                         sema::error::ILLEGAL_REFERENCE_FIELD,
                         std::pair{0UZ, 18UZ}});
}

TEST_CASE("C-ABI extern signatures and aggregates are accepted") {
    helpers::resolve_and_check("extern const ok: fn(x: u32, y: usize, z: isize): f64;");
    helpers::resolve_and_check("const S = extern struct { a: i64, b: bool };");
#if GHOTI_ASM_HOST_X86_64
    helpers::resolve_and_check("extern const with_f80: fn(x: f80): void;");
#endif
}

namespace {

// A target whose C ABI lowering ghoti doesn't model
constexpr std::string_view UNMODELLED{"mips-unknown-linux-gnu"};

constexpr std::string_view PAIR{"const S = extern struct { a: i32, b: i32 };\n"};

auto check_unmodelled_rejection(std::string_view body, std::string_view message) -> void {
    const auto source{fmt::format("{}{}", PAIR, body)};
    auto [ctx, idx]{helpers::resolve_for_target(source, UNMODELLED)};
    const auto& diags{UNWRAP(ctx->root_mod.diagnostics.as_opt<sema::diagnostics>())};
    REQUIRE_FALSE(diags.empty());
    CHECK(diags[0].get_error() == sema::error::UNSUPPORTED_TARGET);
    CHECK(diags[0].get_message().value_or("") == message);
}

[[nodiscard]] auto unmodelled_message(std::string_view signature, std::string_view offender)
    -> std::string {
    return fmt::format("'{}' passes '{}' by value, but ghoti doesn't know how C passes aggregates "
                       "on 'mips-unknown-linux-gnu' yet; pass a pointer to it instead",
                       signature,
                       offender);
}

} // namespace

TEST_CASE("Unmodelled targets reject aggregates passed by value to or from C") {
    check_unmodelled_rejection("extern const take: fn(s: S): i32;",
                               unmodelled_message("fn(S): i32", "S"));
    check_unmodelled_rejection("extern const give: fn(): S;", unmodelled_message("fn(): S", "S"));
    check_unmodelled_rejection("extern const write: fn(bytes: []u8): void;",
                               unmodelled_message("fn([]u8): void", "[]u8"));
    check_unmodelled_rejection("export const take = fn(s: S): i32 { return s.a; };",
                               unmodelled_message("fn(S): i32", "S"));
    check_unmodelled_rejection("const take = fn(s: S): i32 { return s.a; };\n"
                               "@export(take, .{ .name = \"take\" });",
                               unmodelled_message("fn(S): i32", "S"));
    check_unmodelled_rejection(
        "const apply = fn(cb: extern fn(s: S): i32): i32 { _ = cb; return 0; };",
        unmodelled_message("fn(S): i32", "S"));
}

TEST_CASE("Unmodelled targets still compile ghoti-only aggregate passing") {
    const auto source{fmt::format("{}{}", PAIR, R"(
        const sum = fn(xs: []i32): i32 {
            let mut total: i32 = 0;
            for (xs) |x| { total += x; }
            return total;
        };
        const swap = fn(s: S): S { return .{ .a = s.b, .b = s.a }; };
        extern const by_pointer: fn(s: ^S, n: i32): i32;
        export const scalars = fn(a: i32, b: i32): i32 { return a + b; };
        const run = fn(): i32 {
            const items: [3]i32 = .{ 1, 2, 3 };
            const s = swap(.{ .a = 1, .b = 2 });
            return sum(items[0..]) + s.a;
        };
    )")};
    auto [ctx, idx]{helpers::type_check_for_target(source, UNMODELLED)};
    helpers::check_errors<sema::diagnostics>(ctx->root_mod);
}

TEST_CASE("Modelled targets accept aggregates passed by value to C") {
    for (const std::string_view triple : {"x86_64-pc-windows-msvc",
                                          "aarch64-unknown-linux-gnu",
                                          "i686-unknown-linux-gnu",
                                          "armv7-unknown-linux-gnueabihf",
                                          "riscv64-unknown-linux-gnu",
                                          "riscv32-unknown-elf"}) {
        INFO(triple);
        const auto source{fmt::format("{}{}", PAIR, R"(
            extern const take: fn(s: S, bytes: []u8): S;
            export const give = fn(s: S): S { return s; };
        )")};
        auto [ctx, idx]{helpers::resolve_for_target(source, triple)};
        helpers::check_errors<sema::diagnostics>(ctx->root_mod);
    }
}

} // namespace ghoti::tests
