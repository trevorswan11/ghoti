#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view SIZED{R"(
    constexpr N := 3;
    pub const Arr := [N]u32;
    pub const Str := [N:0]u8;
    pub const S := struct { pub d: [N]u64, pub x: u8, };
    pub constexpr Gen := fn(T: type): type { return [N]T; };
)"};

} // namespace

// The leading constants give the root's AST early integer literals, so folding the imported
// dimension node against the root module used to silently read one of them instead
TEST_CASE("E2E: layout builtins on an imported `[N]T` fold N in the declaring module") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            constexpr P := 7;
            constexpr Q := 9;
            constexpr N := 2;
            import "sized.gh" as a;

            const arr: [@sizeOf(a.Arr)]u8 = undefined;
            const bits: [@bitSizeOf(a.Arr)]u8 = undefined;
            const str: [@sizeOf(a.Str)]u8 = undefined;
            const gen: [@sizeOf(a.Gen(u16))]u8 = undefined;
            const local: [@sizeOf([N]u8)]u8 = undefined;

            pub const main := fn(): i32 {
                if (arr.len != 12) { return 1; }
                if (bits.len != 96) { return 2; }
                if (str.len != 4) { return 3; }
                if (gen.len != 6) { return 4; }
                if (local.len != 2) { return 5; }
                if constexpr (@sizeOf(a.Arr) != 12) { return 6; }
                const rt: usize = @sizeOf(a.Arr);
                if (rt != 12) { return 7; }
                return 0;
            };
        )",
        {helpers::mock_file{"sized.gh", SIZED, "sized"}})};

    CHECK(exit_code == 0);
}

TEST_CASE("E2E: layout builtins on an imported struct with a `[N]T` field") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            constexpr P := 7;
            constexpr Q := 9;
            import "sized.gh" as a;

            const size: [@sizeOf(a.S)]u8 = undefined;
            const alignment: [@alignOf(a.S)]u8 = undefined;

            pub const main := fn(): i32 {
                if (size.len != 32) { return 1; }
                if (alignment.len != 8) { return 2; }
                return 0;
            };
        )",
        {helpers::mock_file{"sized.gh", SIZED, "sized"}})};

    CHECK(exit_code == 0);
}

TEST_CASE("E2E: layout builtins on a local struct whose `[N]T` field is still deferred") {
    const auto exit_code{helpers::compile_and_run(R"(
        constexpr N := 3;
        const S := struct { pub d: [N]u64, pub x: u8, };

        const size: [@sizeOf(S)]u8 = undefined;
        const alignment: [@alignOf(S)]u8 = undefined;
        const str: [@sizeOf([N:0]u8)]u8 = undefined;

        pub const main := fn(): i32 {
            if (size.len != 32) { return 1; }
            if (alignment.len != 8) { return 2; }
            if (str.len != 4) { return 3; }
            return 0;
        };
    )")};

    CHECK(exit_code == 0);
}

TEST_CASE("E2E: reflection builtins fold an imported `[N]T` in the declaring module") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            constexpr P := 7;
            constexpr Q := 9;
            import "sized.gh" as a;

            const same := fn(x: []u8, e: []u8): bool {
                if (x.len != e.len) { return false; }
                for (0..x.len) |i| { if (x[i] != e[i]) { return false; } }
                return true;
            };

            const info_len: [@typeInfo(a.Arr).array.len]u8 = undefined;
            const field_len: [@typeInfo(@fieldType(a.S, "d")).array.len]u8 = undefined;
            const name_len: [@typeName(a.Arr).len]u8 = undefined;

            pub const main := fn(): i32 {
                if (info_len.len != 3) { return 1; }
                if (field_len.len != 3) { return 2; }
                if (name_len.len != 6) { return 3; }
                const n := @typeName(a.Arr);
                if (!same(n[0..n.len], "[3]u32")) { return 4; }
                const p := @typeName(^a.Arr);
                if (!same(p[0..p.len], "^[3]u32")) { return 5; }
                return 0;
            };
        )",
        {helpers::mock_file{"sized.gh", SIZED, "sized"}})};

    CHECK(exit_code == 0);
}

TEST_CASE("E2E: builtins see through a concrete array with a deferred `[M]T` element") {
    const auto exit_code{helpers::compile_and_run(R"(
        constexpr N := 3;

        const same := fn(x: []u8, e: []u8): bool {
            if (x.len != e.len) { return false; }
            for (0..x.len) |i| { if (x[i] != e[i]) { return false; } }
            return true;
        };

        const size: [@sizeOf([N][2]u16)]u8 = undefined;
        const inner: [@typeInfo(@typeInfo([N][2]u16).array.child).array.len]u8 = undefined;

        pub const main := fn(): i32 {
            if (size.len != 12) { return 1; }
            if (inner.len != 2) { return 2; }
            const n := @typeName([N][2]u8);
            if (!same(n[0..n.len], "[3][2]u8")) { return 3; }
            const r: usize = @sizeOf([2][N]u32);
            if (r != 24) { return 4; }
            return 0;
        };
    )")};

    CHECK(exit_code == 0);
}

} // namespace ghoti::tests
