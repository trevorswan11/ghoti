#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

using helpers::mock_file;

TEST_CASE("A struct literal applies `= default` for every omitted field") {
    CHECK(helpers::compile_and_run(R"(
        const P := struct { pub a: i32, pub b: i32 = 42, pub c: i32 = 7 };
        pub const main := fn(): i32 {
            var p := P{ .a = 1 };
            return p.a + p.b + p.c;
        };
    )") == 50);
}

TEST_CASE("An explicitly-provided field overrides its default") {
    CHECK(helpers::compile_and_run(R"(
        const P := struct { pub a: i32 = 1, pub b: i32 = 2 };
        pub const main := fn(): i32 {
            var p := P{ .b = 99 };
            return p.a + p.b;
        };
    )") == 100);
}

TEST_CASE("Field defaults survive a `.{ ... }` implicit-type literal") {
    CHECK(helpers::compile_and_run(R"(
        const Cfg := struct { pub retries: i32 = 3, pub verbose: bool = true };
        const use_cfg := fn(c: Cfg): i32 {
            return c.retries + (if (c.verbose) 10 else 0);
        };
        pub const main := fn(): i32 {
            return use_cfg(.{});
        };
    )") == 13);
}

TEST_CASE("A defaulted slice field is initialized, not garbage") {
    CHECK(helpers::compile_and_run(R"(
        const Buf := struct { pub data: []mut u8, pub pos: usize = 0 };
        const room := fn(b: &Buf): usize { return b.data.len - b.pos; };
        pub const main := fn(): i32 {
            var backing: [16]mut u8 = undefined;
            var b := Buf{ .data = backing };
            return @as(i32, b.pos) * 100 + @as(i32, room(&b));
        };
    )") == 16);
}

TEST_CASE("A field default that references another module resolves") {
    constexpr std::string_view opts_gh{R"(
        pub const LIMIT := 256;
        pub const Options := struct { pub name: []u8, pub cap: i32 = LIMIT };
    )"};

    CHECK(helpers::compile_and_run(
              R"(
            import "opts.gh" as opts;
            pub const main := fn(): i32 {
                var o := opts::Options{ .name = "x" };
                return o.cap - 200;
            };
        )",
              {mock_file{"opts.gh", opts_gh, "opts"}}) == 56);
}

} // namespace ghoti::tests
