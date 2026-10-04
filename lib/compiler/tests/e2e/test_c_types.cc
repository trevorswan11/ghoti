#include <string>
#include <string_view>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/types.hh>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

// Asserts each C type's layout at compile time for one target
[[nodiscard]] auto layout_assertions(u32 long_bits, bool char_signed, u32 longdouble_bits)
    -> std::string {
    return fmt::format(R"(
        comptime {{
            @assert(@typeInfo(c_char).int.bits == 8);
            @assert(@typeInfo(c_char).int.signedness == .{0});
            @assert(@typeInfo(c_short).int.bits == 16 and @typeInfo(c_ushort).int.bits == 16);
            @assert(@typeInfo(c_int).int.bits == 32 and @typeInfo(c_uint).int.bits == 32);
            @assert(@typeInfo(c_long).int.bits == {1} and @typeInfo(c_ulong).int.bits == {1});
            @assert(@typeInfo(c_longlong).int.bits == 64);
            @assert(@typeInfo(c_ulonglong).int.signedness == .unsigned);
            @assert(@typeInfo(c_longdouble).float.bits == {2});
        }}
    )",
                       char_signed ? "signed" : "unsigned",
                       long_bits,
                       longdouble_bits);
}

} // namespace

TEST_CASE("C types are sized and signed as the target's C ABI says") {
    const auto check_layout{[](const std::string& source, std::string_view triple) {
        INFO(triple);
        const auto [ctx, idx]{helpers::type_check_for_target(source, triple)};
        helpers::check_errors<sema::diagnostics>(ctx->root_mod);
    }};
    check_layout(layout_assertions(64, true, 80), "x86_64-unknown-linux-gnu");
    check_layout(layout_assertions(32, true, 64), "x86_64-pc-windows-msvc");
    check_layout(layout_assertions(64, false, 128), "aarch64-unknown-linux-gnu");
    check_layout(layout_assertions(64, true, 64), "arm64-apple-macos");
    check_layout(layout_assertions(64, false, 128), "riscv64-unknown-linux-gnu");
    check_layout(layout_assertions(32, true, 128), "wasm32-unknown-unknown");
}

TEST_CASE("A C type converts implicitly only where every value fits") {
    CHECK(helpers::compile_and_run(R"(
        const widen = fn(a: c_uint): i64 { return a; };
        const promote = fn(a: c_short): c_int { return a; };
        pub const main = fn(): i32 {
            let a: c_int = 5;
            let b: i32 = a;
            let c: c_int = b;
            let d: i64 = a;
            return b + @as(i32, @intCast(d)) + c + @as(i32, @intCast(widen(3))) + promote(4);
        };
    )") == 5 + 5 + 5 + 3 + 4);

    const auto mismatch{[](std::string_view body) {
        helpers::expect_compile_error(
            fmt::format("{}\npub const main = fn(): i32 {{ return 0; }};", body));
    }};
    mismatch("const f = fn(a: c_int): c_uint { return a; };");
    mismatch("const f = fn(a: c_uint): i32 { return a; };");
    mismatch("const f = fn(a: c_longlong): c_int { return a; };");
}

TEST_CASE("A C type is its own type, not the fixed-width integer of its size") {
    CHECK(helpers::compile_and_run(R"(
        const size = fn(x: auto): i32 { return @intCast(@sizeOf(@TypeOf(x))); };
        pub const main = fn(): i32 {
            let a: c_int = 1;
            let b: i32 = 1;
            if (c_int == i32) { return 99; }
            return size(a) + size(b);
        };
    )") == 8);
    // Pointers to them don't convert, as in C
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 { let a: c_int = 5; let p: ^i32 = ^a; return 0; };
    )");
}

TEST_CASE("`c_longdouble` is its own type, holding the same values as the float it's sized as") {
    // Checked at compile time, since a runtime `f128` add may need compiler_rt on the host
    helpers::type_check_and_verify(R"(
        comptime {
            @assert(c_longdouble != f64 and c_longdouble != f80 and c_longdouble != f128);
            const a: c_longdouble = 2.5;
            const d: f64 = 1.5;
            const e: c_longdouble = d;
            @assert(a + e == 4.0);
        }
    )");
    // Pointers to it don't convert to pointers to the float it's sized as
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            let a: c_longdouble = 2.5;
            let p: ^f64 = ^a;
            let q: ^f80 = ^a;
            let r: ^f128 = ^a;
            return 0;
        };
    )");
}

} // namespace ghoti::tests
