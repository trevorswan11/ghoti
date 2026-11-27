#include <string>
#include <string_view>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "compiler/sema/error.hh"

#include "helpers/uir_diff.hh"

namespace ghoti::tests {

namespace {

struct rejected {
    std::string_view source;
    std::string_view message;
};

} // namespace

TEST_CASE("UIR Sema accepts the call modifiers a call can honor", "[uir]") {
    const auto run{helpers::run_uir_sema(R"(
        pub const add = fn(a: i32, b: i32): i32 { return a + b; };
        pub const no_tail = fn(): i32 { return @call(.never_tail, add, 1, 2); };
        pub const no_inline = fn(): i32 { return @call(.never_inline, add, 1, 2); };
        pub const inlined = fn(): i32 { return @call(.always_inline, add, 1, 2); };
        pub const tail = fn(a: i32, b: i32): i32 { return @call(.always_tail, add, a, b); };
    )")};
    REQUIRE_FALSE(run->out->stopped_at);
    CHECK(run->out->succeeded());
}

TEST_CASE("UIR Sema rejects a call modifier its call can't honor", "[uir]") {
    const auto [source, message]{
        GENERATE(rejected{.source  = R"(
            const f = @[inline(.never)] fn(x: i32): i32 { return x; };
            pub const g = fn(): i32 { return @call(.always_inline, f, 1); };
        )",
                          .message = "the function is `@[inline(.never)]`"},
                 rejected{.source  = R"(
            const f = @[inline(.always)] fn(x: i32): i32 { return x; };
            pub const g = fn(): i32 { return @call(.never_inline, f, 1); };
        )",
                          .message = "the function is `@[inline(.always)]`"},
                 rejected{.source  = R"(
            let mut counter: i32 = 0;
            const f = fn(x: i32): i32 { return x; };
            pub const g = fn(x: i32): i32 {
                defer { counter += 1; }
                return @call(.always_tail, f, x);
            };
        )",
                          .message = "the call must be returned directly"},
                 rejected{.source  = R"(
            const f = fn(x: i32): i64 { return @intCast(i64, x); };
            pub const g = fn(x: i32): i32 { return @intCast(i32, @call(.always_tail, f, x)); };
        )",
                          .message = "the call must be returned directly"},
                 rejected{.source  = R"(
            const f = fn(x: i64): i32 { return @intCast(i32, x); };
            pub const g = fn(x: i32): i32 { return @call(.always_tail, f, @intCast(i64, x)); };
        )",
                          .message = "needs the same parameters, result, and calling convention"})};
    INFO(source);

    const auto run{helpers::run_uir_sema(source)};
    REQUIRE_FALSE(run->out->stopped_at);
    REQUIRE_FALSE(run->out->succeeded());
    REQUIRE(run->out->diagnostics.size() == 1);
    const auto& diag{run->out->diagnostics[0]};
    CHECK(diag.get_error() == sema::error::IMPOSSIBLE_CALL_MODIFIER);
    CHECK(std::string{diag.get_message().value_or("")}.contains(message));
}

} // namespace ghoti::tests
