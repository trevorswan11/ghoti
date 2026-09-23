#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view SIZED{R"(
    constexpr N := 3;
    pub using Arr = [N]u32;
    pub using Str = [N:0]u8;
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

} // namespace ghoti::tests
