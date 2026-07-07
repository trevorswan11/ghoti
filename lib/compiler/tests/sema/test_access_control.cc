#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

using helpers::mock_file;

TEST_CASE("Cross-module public vs private declaration access") {
    constexpr std::string_view math_gh{R"(
        pub const add := fn(a: i32, b: i32): i32 { return a + b; };
        const secret := 42;
    )"};

    SECTION("Public function access succeeds") {
        auto [ctx, idx]{
            helpers::resolve_and_check(R"(import "math.gh" as math; const res := math::add(1, 2);)",
                                       {mock_file{.path = "math.gh", .source = math_gh}})};
        ctx->verify_registry_resolved();
    }

    SECTION("Private declaration access fails") {
        helpers::test_resolver_fail(R"(import "math.gh" as math; const res := math::secret;)",
                                    {mock_file{.path = "math.gh", .source = math_gh}},
                                    sema::diagnostic{
                                        "Symbol 'secret' is private to module 'math'",
                                        sema::error::ILLEGAL_PRIVATE_ACCESS,
                                        std::pair{1UZ, 20UZ},
                                    });
    }
}

TEST_CASE("Cross-module public vs private struct field access") {
    constexpr std::string_view point_gh{R"(
        pub const Point := struct {
            pub x: i32,
            y: i32,
        };
        pub const make_point := fn(): Point {
            return Point{ .x = 10, .y = 20 };
        };
    )"};

    SECTION("Public field access succeeds") {
        auto [ctx, idx]{helpers::resolve_and_check(
            R"(import "point.gh" as point;
               const p := point::make_point();
               const px := p.x;)",
            {mock_file{.path = "point.gh", .source = point_gh}})};
        ctx->verify_registry_resolved();
    }

    SECTION("Private struct field access across modules fails") {
        helpers::test_resolver_fail(
            R"(import "point.gh" as point;
               const p := point::make_point();
               const py := p.y;)",
            {mock_file{.path = "point.gh", .source = point_gh}},
            sema::diagnostic{
                "Field 'y' of struct 'p' is private",
                sema::error::ILLEGAL_PRIVATE_ACCESS,
                std::pair{2UZ, 15UZ},
            });
    }
}

TEST_CASE("Cross-module re-exported symbol access") {
    constexpr std::string_view io_gh{R"(
        pub const println := fn(s: []u8): void {};
    )"};

    constexpr std::string_view std_pub_gh{R"(
        pub import "io.gh" as io;
    )"};

    constexpr std::string_view std_priv_gh{R"(
        import "io.gh" as io;
    )"};

    SECTION("Re-exported public import succeeds") {
        auto [ctx, idx]{
            helpers::resolve_and_check(R"(import "std.gh" as std; const f := std::io::println;)",
                                       {mock_file{.path = "std.gh", .source = std_pub_gh},
                                        mock_file{.path = "io.gh", .source = io_gh}})};
        ctx->verify_registry_resolved();
    }

    SECTION("Private import access from outer module fails") {
        helpers::test_resolver_fail(R"(import "std.gh" as std; const f := std::io::println;)",
                                    {mock_file{.path = "std.gh", .source = std_priv_gh},
                                     mock_file{.path = "io.gh", .source = io_gh}},
                                    sema::diagnostic{
                                        "Symbol 'io' is private to module 'std'",
                                        sema::error::ILLEGAL_PRIVATE_ACCESS,
                                        std::pair{1UZ, 17UZ},
                                    });
    }
}

} // namespace ghoti::tests
