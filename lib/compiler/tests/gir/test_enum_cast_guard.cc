#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/gir.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("GIR guards an integer -> exhaustive-enum cast with a panic") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Color := enum { red, green, blue };
        const pick := fn(n: i32): Color { return @as(Color, n); };
    )")};

    const auto dump_text{helpers::dump_named_fn(*ctx, "pick")};
    CHECK(dump_text.contains("cond_goto"));
    CHECK(dump_text.contains("panic_handler"));
}

TEST_CASE("GIR does not guard a cast to a non-exhaustive enum") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Color := enum { red, green, blue, _ };
        const pick := fn(n: i32): Color { return @as(Color, n); };
    )")};

    const auto dump_text{helpers::dump_named_fn(*ctx, "pick")};
    CHECK_FALSE(dump_text.contains("panic_handler"));
}

TEST_CASE("GIR omits the guard when the value is a known-good constant") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Color := enum { red, green, blue };
        const pick := fn(): Color { return @as(Color, 1); };
    )")};

    const auto dump_text{helpers::dump_named_fn(*ctx, "pick")};
    CHECK_FALSE(dump_text.contains("panic_handler"));
}

} // namespace ghoti::tests
