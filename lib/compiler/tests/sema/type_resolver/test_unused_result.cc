#include <algorithm>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <gsl/span>

#include "compiler/sema/error.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

// Whether resolving `source` produced at least one diagnostic with the given error code.
[[nodiscard]] auto has_error(std::string_view source, sema::error code) -> bool {
    const auto [codes, _]{helpers::resolve_diags(source)};
    return std::ranges::contains(codes, code);
}

} // namespace

TEST_CASE("A dropped non-void call result is an error under --unused-result=error") {
    CHECK(has_error(R"(
        const make := fn(): i32 { return 7; };
        pub const main := fn(): i32 {
            make();
            return 0;
        };
    )",
                    sema::error::UNUSED_RESULT));
}

TEST_CASE("Consuming a call result satisfies the must-use check") {
    const auto ok{[](std::string_view body) {
        auto [ctx, idx]{helpers::resolve(body)};
        helpers::check_errors<sema::diagnostics>(ctx->root_mod);
    }};

    SECTION("_ = discards it") {
        ok(R"(
            const make := fn(): i32 { return 7; };
            pub const main := fn(): i32 { _ = make(); return 0; };
        )");
    }
    SECTION("bound to a const") {
        ok(R"(
            const make := fn(): i32 { return 7; };
            pub const main := fn(): i32 { const x := make(); return x; };
        )");
    }
    SECTION("a void call needs nothing") {
        ok(R"(
            const noop := fn(): void {};
            pub const main := fn(): i32 { noop(); return 0; };
        )");
    }
}

TEST_CASE("A @[discardable] callee may be dropped with no diagnostic") {
    const auto ok{[](std::string_view body) {
        auto [ctx, idx]{helpers::resolve(body)};
        helpers::check_errors<sema::diagnostics>(ctx->root_mod);
    }};

    SECTION("direct call") {
        ok(R"(
            @[discardable] const log := fn(n: i32): i32 { return n; };
            pub const main := fn(): i32 { log(3); return 0; };
        )");
    }
    SECTION("attribute list on its own line") {
        ok(R"(
            @[discardable,]
            const log := fn(n: i32): i32 { return n; };
            pub const main := fn(): i32 { log(3); return 0; };
        )");
    }
    SECTION("through a direct alias") {
        ok(R"(
            @[discardable] const log := fn(n: i32): i32 { return n; };
            const note := log;
            pub const main := fn(): i32 { note(3); return 0; };
        )");
    }
    SECTION("on the function literal itself") {
        ok(R"(
            const log := @[discardable] fn(n: i32): i32 { return n; };
            pub const main := fn(): i32 { log(3); return 0; };
        )");
    }
    SECTION("on an extern function declaration") {
        ok(R"(
            @[discardable] extern const abs: extern fn(n: i32): i32;
            pub const main := fn(): i32 { abs(3); return 0; };
        )");
    }
    SECTION("method call on struct") {
        ok(R"(
            const S := struct {
                @[discardable] pub const close := fn(&self): i32 { return 0; };
            };
            pub const main := fn(): i32 {
                const s: S = .{};
                s.close();
                return 0;
            };
        )");
    }
}

TEST_CASE("@[discardable] is rejected where it cannot apply") {
    SECTION("on a non-function declaration") {
        CHECK(has_error("@[discardable] const X: i32 = 3;", sema::error::ILLEGAL_ATTRIBUTE));
    }
    SECTION("on a function that returns void") {
        CHECK(has_error("@[discardable] const f := fn(): void {};", sema::error::ILLEGAL_ATTRIBUTE));
    }
    SECTION("on a void function literal") {
        CHECK(has_error("const f := @[discardable] fn(): void {};", sema::error::ILLEGAL_ATTRIBUTE));
    }
}

TEST_CASE("@[discardable(<constexpr bool>)] gates the discard behavior on the condition") {
    const auto ok{[](std::string_view body) {
        auto [ctx, idx]{helpers::resolve(body)};
        helpers::check_errors<sema::diagnostics>(ctx->root_mod);
    }};

    SECTION("a true condition allows the drop") {
        ok(R"(
            constexpr ON := true;
            @[discardable(ON)] const log := fn(n: i32): i32 { return n; };
            pub const main := fn(): i32 { log(3); return 0; };
        )");
    }
    SECTION("a false condition keeps the must-use error") {
        CHECK(has_error(R"(
            constexpr OFF := false;
            @[discardable(OFF)] const log := fn(n: i32): i32 { return n; };
            pub const main := fn(): i32 { log(3); return 0; };
        )",
                        sema::error::UNUSED_RESULT));
    }
    SECTION("a false condition on a function literal keeps the must-use error") {
        CHECK(has_error(R"(
            const log := @[discardable(false)] fn(n: i32): i32 { return n; };
            pub const main := fn(): i32 { log(3); return 0; };
        )",
                        sema::error::UNUSED_RESULT));
    }
    SECTION("a non-boolean condition is an error") {
        CHECK(has_error("@[discardable(1 + 1)] const f := fn(): i32 { return 0; };",
                        sema::error::ILLEGAL_ATTRIBUTE));
    }
}

} // namespace ghoti::tests
