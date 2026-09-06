#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("E2E cfg: a module-scope @cfg picks exactly one declaration") {
    CHECK(helpers::compile_and_run(R"(
        @cfg(ptr_bits >= 16) { const N := 42; }
        else                 { const N := 7; }

        pub const main := fn(): i32 { return N; };
    )") == 42);
}

TEST_CASE("E2E cfg: an else-@cfg chain falls through to the final else") {
    CHECK(helpers::compile_and_run(R"(
        @cfg(ptr_bits == 7)      { const N := 1; }
        else @cfg(ptr_bits == 9) { const N := 2; }
        else                     { const N := 9; }

        pub const main := fn(): i32 { return N; };
    )") == 9);
}

TEST_CASE("E2E cfg: a @cfg statement gates control flow inside a function body") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            @cfg(ptr_bits == 64) { return 64; }
            else                 { return 32; }
        };
    )") == 64);
}

TEST_CASE("E2E cfg: a @cfgValue predicate constant is readable as a constexpr bool") {
    CHECK(helpers::compile_and_run(R"(
        const IS_WIDE := @cfgValue(ptr_bits >= 32);

        pub const main := fn(): i32 {
            if constexpr (IS_WIDE) { return 1; }
            else                   { return 0; }
        };
    )") == 1);
}

TEST_CASE("E2E cfg: @cfgValue constants may chain acyclically and gate a @cfg block") {
    CHECK(helpers::compile_and_run(R"(
        const A := @cfgValue(ptr_bits >= 8);
        const B := @cfgValue(A);
        const C := @cfgValue(A and B);

        @cfg(C) { const R := 5; } else { const R := 0; }

        pub const main := fn(): i32 { return R; };
    )") == 5);
}

TEST_CASE("E2E cfg: the @cfgValue guard form yields a per-target constexpr value") {
    CHECK(helpers::compile_and_run(R"(
        const WORD := @cfgValue(
            ptr_bits == 64 => 8,
            ptr_bits == 32 => 4,
            _              => 2,
        );

        pub const main := fn(): i32 { return WORD * 5; };
    )") == 40);
}

namespace {

constexpr std::string_view GOOD_BACKEND{R"(
    pub const value := fn(): i32 { return 77; };
)"};

constexpr std::string_view BROKEN_BACKEND{R"(
    this is not valid ghoti @@@ !!!
)"};

} // namespace

TEST_CASE("E2E cfg: a non-selected @cfg import is never parsed") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            @cfg(ptr_bits == 64) import "good.gh"   as backend;
            else                 import "broken.gh" as backend;

            pub const main := fn(): i32 { return backend::value(); };
        )",
        {
            helpers::mock_file{"good.gh", GOOD_BACKEND, "good"},
            helpers::mock_file{"broken.gh", BROKEN_BACKEND, "broken"},
        })};

    CHECK(exit_code == 77);
}

TEST_CASE("E2E cfg: a selected struct @cfg block contributes real, ordered fields") {
    CHECK(helpers::compile_and_run(R"(
        const S := struct {
            a: i32,
            @cfg(ptr_bits >= 16) { b: i32, c: i32 }
            d: i32,
        };

        pub const main := fn(): i32 {
            const s := S{ .a = 1, .b = 2, .c = 3, .d = 4 };
            return s.a + s.b + s.c + s.d;
        };
    )") == 10);
}

TEST_CASE("E2E cfg: @cfg selects an aggregate member and its methods stay callable") {
    CHECK(helpers::compile_and_run(R"(
        const S := struct {
            n: i32,
            @cfg(ptr_bits == 64) { const scale := fn(v: i32): i32 { return v * 2; }; }
            else                 { const scale := fn(v: i32): i32 { return v * 3; }; }
        };

        pub const main := fn(): i32 { return S.scale(21); };
    )") == 42);
}

TEST_CASE("E2E cfg: an unselected struct @cfg block drops its fields; else wins") {
    CHECK(helpers::compile_and_run(R"(
        const S := struct {
            a: i32,
            @cfg(ptr_bits == 7) { unused: i32 }
            else                { fallback: i32 }
        };

        pub const main := fn(): i32 {
            const s := S{ .a = 40, .fallback = 2 };
            return s.a + s.fallback;
        };
    )") == 42);
}

TEST_CASE("E2E cfg: a non-generic `if constexpr` over a @cfgValue prunes the dead arm") {
    CHECK(helpers::compile_and_run(R"(
        const IS_64 := @cfgValue(ptr_bits == 64);

        pub const main := fn(): i32 {
            if constexpr (IS_64) { return 123; }
            else                 { return never_declared_symbol + also_never; }
        };
    )") == 123);
}

TEST_CASE("E2E cfg: a generic `if constexpr` prunes the dead arm per instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const pick := fn(T: type, x: T): i32 {
            if constexpr (T == i32) { return 100; }
            else                    { return x.no_such_method_on_this_type(); }
        };

        pub const main := fn(): i32 { return pick(i32, 5); };
    )") == 100);
}

TEST_CASE("E2E cfg: a valued `if constexpr` over a @cfgValue takes the live arm's type") {
    CHECK(helpers::compile_and_run(R"(
        const IS_64 := @cfgValue(ptr_bits == 64);

        pub const main := fn(): i32 {
            const n := if constexpr (IS_64) 7 else true;
            return n + 35;
        };
    )") == 42);
}

TEST_CASE("E2E cfg: a non-taken @cfgValue guard arm with @compileError is inert") {
    CHECK(helpers::compile_and_run(R"(
        const SYS := @cfgValue(
            ptr_bits == 64 => 30,
            _              => @compileError("unsupported pointer width"),
        );

        pub const main := fn(): i32 { return SYS + 12; };
    )") == 42);
}

TEST_CASE("E2E cfg: a @cfg-gated enum variant is real and usable") {
    CHECK(helpers::compile_and_run(R"(
        const Tag := enum {
            A,
            @cfg(ptr_bits >= 16) { B, C = 42 }
            D,
        };

        pub const main := fn(): i32 {
            const t := Tag.C;
            return match (t) {
                .A => 0,
                .B => 1,
                .C => |c| @as(i32, c),
                .D => 3,
            };
        };
    )") == 42);
}

TEST_CASE("E2E cfg: a non-braced @cfg arm on a variant / field may be followed by a bare else") {
    CHECK(helpers::compile_and_run(R"(
        const Tag := enum : u32 {
            A = 1u32,
            @cfg(ptr_bits >= 16) TAKEN = 64u32,
            else DEAD = 32u32,
            _,
        };
        pub const main := fn(): i32 { return @as(i32, @as(u32, Tag.TAKEN)); };
    )") == 64);

    CHECK(helpers::compile_and_run(R"(
        const S := struct {
            a: i32,
            @cfg(ptr_bits >= 16) w: i32,
            else dead: i64,
            b: i32,
        };
        pub const main := fn(): i32 {
            const s: S = .{ .a = 1, .w = 5, .b = 1 };
            return s.w + s.a + s.b;
        };
    )") == 7);
}

TEST_CASE("E2E cfg: a `@cfg`-gated member may follow a non-exhaustive enum's `_` marker") {
    CHECK(helpers::compile_and_run(R"(
        const E := enum : u32 {
            A = 1u32,
            _,
            @cfg(ptr_bits >= 16) pub const label := fn(): i32 { return 7; };
            else pub const label := fn(): i32 { return 3; };
        };
        pub const main := fn(): i32 { return E.label(); };
    )") == 7);
}

TEST_CASE("E2E cfg: a re-exported @cfgValue constant is usable cross-module as a value") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "sys.gh" as sys;
            pub const main := fn(): i32 {
                var buf: [sys::word]mut i32 = undefined;
                buf[sys::word - 1uz] = 3;
                const via_if := if constexpr (sys::is_wide) 4 else 2;
                const via_val := sys::is_wide;
                return buf[sys::word - 1uz] + via_if + (if (via_val) 1 else 0);
            };
        )",
        {helpers::mock_file{"sys.gh",
                            R"(
            pub constexpr is_wide := @cfgValue(ptr_bits >= 32);
            pub constexpr word := @cfgValue(
                ptr_bits == 64 => 8uz,
                ptr_bits == 32 => 4uz,
                _              => @compileError("unsupported"),
            );
        )",
                            "sys"}})};
    CHECK(exit_code == 8);
}

TEST_CASE("E2E cfg: endian is decided at compile time and exactly one arm runs") {
    CHECK(helpers::compile_and_run(R"(
        @cfg(endian == .little) { const MARK := 1; }
        else                    { const MARK := 2; }

        pub const main := fn(): i32 {
            @cfg(endian == .big) { return 20; }
            else                 { return MARK + 40; }
        };
    )") == 41);
}

} // namespace ghoti::tests
