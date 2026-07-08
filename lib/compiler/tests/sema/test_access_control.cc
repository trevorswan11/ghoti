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
                                        std::pair{0UZ, 45UZ},
                                    });
    }
}

TEST_CASE("Cross-module public vs private struct field and member access") {
    constexpr std::string_view point_gh{R"(
        pub const Point := struct {
            pub x: i32,
            y: i32,
            pub const get_origin := fn(): i32 { return 0; };
            const secret_helper := fn(): i32 { return -1; };
        };
        pub const make_point := fn(): Point {
            return Point{ .x = 10, .y = 20 };
        };
    )"};

    SECTION("Public field and member access succeeds") {
        auto [ctx, idx]{helpers::resolve_and_check(
            R"(import "point.gh" as point;
               const p := point::make_point();
               const px := p.x;
               const zero_fn := point::Point.get_origin;)",
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
                std::pair{2UZ, 30UZ},
            });
    }

    SECTION("Private struct member access across modules fails") {
        helpers::test_resolver_fail(
            R"(import "point.gh" as point; const p := point::Point.secret_helper;)",
            {mock_file{.path = "point.gh", .source = point_gh}},
            sema::diagnostic{
                "Member 'secret_helper' of struct 'Point' is private",
                sema::error::ILLEGAL_PRIVATE_ACCESS,
                std::pair{0UZ, 52UZ},
            });
    }
}

TEST_CASE("Cross-module public vs private enum member access") {
    constexpr std::string_view color_gh{R"(
        pub const Color := enum {
            RED,
            GREEN,
            BLUE,
            pub const get_default := fn(): Color { return Color.RED; };
            const secret_code := fn(): i32 { return 42; };
        };
    )"};

    SECTION("Public enum member access succeeds") {
        auto [ctx, idx]{helpers::resolve_and_check(
            R"(import "color.gh" as color;
               const c := color::Color.get_default;
               const r := color::Color.RED;)",
            {mock_file{.path = "color.gh", .source = color_gh}})};
        ctx->verify_registry_resolved();
    }

    SECTION("Private enum member access across modules fails") {
        helpers::test_resolver_fail(
            R"(import "color.gh" as color; const code := color::Color.secret_code;)",
            {mock_file{.path = "color.gh", .source = color_gh}},
            sema::diagnostic{
                "Member 'secret_code' of enum 'Color' is private",
                sema::error::ILLEGAL_PRIVATE_ACCESS,
                std::pair{0UZ, 55UZ},
            });
    }
}

TEST_CASE("Cross-module public vs private union member access") {
    constexpr std::string_view data_gh{R"(
        pub const Value := union {
            int_val: i32,
            pub const get_zero := fn(): i32 { return 0; };
            const secret_tag := fn(): i32 { return 99; };
        };
    )"};

    SECTION("Public union member access succeeds") {
        auto [ctx, idx]{helpers::resolve_and_check(
            R"(import "data.gh" as data; const z := data::Value.get_zero;)",
            {mock_file{.path = "data.gh", .source = data_gh}})};
        ctx->verify_registry_resolved();
    }

    SECTION("Private union member access across modules fails") {
        helpers::test_resolver_fail(
            R"(import "data.gh" as data; const t := data::Value.secret_tag;)",
            {mock_file{.path = "data.gh", .source = data_gh}},
            sema::diagnostic{
                "Member 'secret_tag' of union 'Value' is private",
                sema::error::ILLEGAL_PRIVATE_ACCESS,
                std::pair{0UZ, 49UZ},
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
                                        std::pair{0UZ, 40UZ},
                                    });
    }
}

} // namespace ghoti::tests
