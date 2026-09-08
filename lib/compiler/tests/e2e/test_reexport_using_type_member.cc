#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

using helpers::mock_file;

constexpr std::string_view ENUM_MOD{R"(
    pub const E := enum : u32 { A = 1u32, B = 2u32, C = 7u32, _ };
    pub using Alias = E;
)"};

constexpr std::string_view MID_MOD{R"(
    import "enums.gh" as en;
    pub using Errno = en::E;
)"};

constexpr std::string_view PKG_MOD{R"(
    pub import "enums.gh" as en;
)"};

constexpr std::string_view AGG_MOD{R"(
    pub const Cfg := struct {
        pub constexpr LIMIT: i32 = 42;
        pub constexpr twice := fn(x: i32): i32 { return x * 2; };
    };
)"};

} // namespace

TEST_CASE("E2E: cross-module `pub using` alias resolves an enum variant via `alias::Type.MEMBER`") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "enums.gh" as x;
            pub const main := fn(): i32 {
                const e: x::Alias = x::Alias.C;
                return if (e == x::Alias.C) @as(i32, @as(u32, x::Alias.B)) + 5 else 1;
            };
        )",
        {mock_file{"enums.gh", ENUM_MOD, "enums"}})};
    CHECK(exit_code == 7);
}

TEST_CASE("E2E: a `pub using` alias re-exported through a middle module still resolves `.MEMBER`") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "mid.gh" as m;
            pub const main := fn(): i32 {
                const e: m::Errno = m::Errno.B;
                return if (e == m::Errno.B) 7 else 1;
            };
        )",
        {
            mock_file{"enums.gh", ENUM_MOD, "enums"},
            mock_file{"mid.gh", MID_MOD, "mid"},
        })};
    CHECK(exit_code == 7);
}

TEST_CASE("E2E: a local `using` alias of a module resolves `alias::Type.MEMBER`") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "pkg.gh" as pkg;
            using x = pkg::en;
            pub const main := fn(): i32 {
                const e: x::E = x::E.C;
                return if (e == x::E.C) @as(i32, @as(u32, x::E.B)) + 5 else 1;
            };
        )",
        {
            mock_file{"enums.gh", ENUM_MOD, "enums"},
            mock_file{"pkg.gh", PKG_MOD, "pkg"},
        })};
    CHECK(exit_code == 7);
}

TEST_CASE("E2E: `alias::Enum.MEMBER` folds through a generic-union `match` capture and compare") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "pkg.gh" as pkg;
            using x = pkg::en;
            constexpr Result := fn(T: type, F: type): type { return union { ok: T, err: F }; };
            const g := fn(): Result(i32, x::E) { return .{ .err = x::E.B }; };
            pub const main := fn(): i32 {
                const miss := match (g()) {
                    .ok  => x::E.A,
                    .err => |e| e,
                };
                return if (miss == x::E.B) 7 else 1;
            };
        )",
        {
            mock_file{"enums.gh", ENUM_MOD, "enums"},
            mock_file{"pkg.gh", PKG_MOD, "pkg"},
        })};
    CHECK(exit_code == 7);
}

TEST_CASE("E2E: a `using` alias of a cross-module struct resolves its `constexpr` static members") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "agg.gh" as agg;
            using C = agg::Cfg;
            pub const main := fn(): i32 {
                return C.twice(20) + @as(i32, C.LIMIT) - 60;
            };
        )",
        {mock_file{"agg.gh", AGG_MOD, "agg"}})};
    CHECK(exit_code == 22);
}

TEST_CASE(
    "E2E: a `match` capture across a cross-module `using` alias binds the payload correctly") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "enums.gh" as x;
            const R := union { ok: x::Alias, err: i32 };
            pub const main := fn(): i32 {
                const r: R = .{ .ok = x::Alias.B };
                return match (r) {
                    .ok  => |v| if (v == x::Alias.B) 7 else 3,
                    .err => |e| e,
                };
            };
        )",
        {mock_file{"enums.gh", ENUM_MOD, "enums"}})};
    CHECK(exit_code == 7);
}

} // namespace ghoti::tests
