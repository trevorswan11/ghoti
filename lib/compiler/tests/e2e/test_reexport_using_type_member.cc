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
