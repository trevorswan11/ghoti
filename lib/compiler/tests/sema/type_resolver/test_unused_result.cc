#include <algorithm>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
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
        const make = fn(): i32 { return 7; };
        pub const main = fn(): i32 {
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
            const make = fn(): i32 { return 7; };
            pub const main = fn(): i32 { _ = make(); return 0; };
        )");
    }
    SECTION("bound to a const") {
        ok(R"(
            const make = fn(): i32 { return 7; };
            pub const main = fn(): i32 { let x = make(); return x; };
        )");
    }
    SECTION("a void call needs nothing") {
        ok(R"(
            const noop = fn(): void {};
            pub const main = fn(): i32 { noop(); return 0; };
        )");
    }
}

TEST_CASE("A dropped non-call value is an error") {
    const auto with_main{[](std::string_view body) {
        return fmt::format(R"(
            const make = fn(): i32 {{ return 7; }};
            pub const main = fn(): i32 {{
                let mut r: i32 = 3;
                {}
                return r;
            }};
        )",
                           body);
    }};

    SECTION("a comparison mistyped for a compound assignment") {
        const auto out{helpers::resolve_diags(with_main("r >= 1;"))};
        CHECK(std::ranges::contains(out.codes, sema::error::UNUSED_RESULT));
        CHECK(out.message_contains("result of this expression is unused; bind it, or `_ =` it"));
    }
    SECTION("arithmetic") { CHECK(has_error(with_main("r + 1;"), sema::error::UNUSED_RESULT)); }
    SECTION("a unary operator") { CHECK(has_error(with_main("-r;"), sema::error::UNUSED_RESULT)); }
    SECTION("a bare name") { CHECK(has_error(with_main("r;"), sema::error::UNUSED_RESULT)); }
    SECTION("a literal") { CHECK(has_error(with_main("5;"), sema::error::UNUSED_RESULT)); }
    SECTION("a call result in an arm of an unused if") {
        CHECK(has_error(with_main("if (r > 0) make();"), sema::error::UNUSED_RESULT));
    }
    SECTION("a value in an arm of an unused match") {
        CHECK(has_error(with_main("match (r) { 0 => r + 1, _ => r = 2, };"),
                        sema::error::UNUSED_RESULT));
    }
}

TEST_CASE("Statements that only do control flow or store need no binding") {
    const auto ok{[](std::string_view body) {
        auto [ctx, idx]{helpers::resolve(fmt::format(R"(
            const noop = fn(): void {{}};
            pub const main = fn(): i32 {{
                let mut r: i32 = 3;
                {}
                return r;
            }};
        )",
                                                     body))};
        helpers::check_errors<sema::diagnostics>(ctx->root_mod);
    }};

    SECTION("an assignment") { ok("r = 1; r -= 1;"); }
    SECTION("an if whose arms store") { ok("if (r > 0) r -= 1 else r += 1;"); }
    SECTION("an if whose arm is a block") { ok("if (r > 0) { r -= 1; }"); }
    SECTION("an if whose arm is a void call") { ok("if (r > 0) noop();"); }
    SECTION("a match whose arms store") { ok("match (r) { 0 => r = 1, _ => noop(), };"); }
    SECTION("a while loop") { ok("while (r > 0) : (r -= 1) {}"); }
    SECTION("a for loop") { ok("for (0..3) |i| { r += @intCast(i); }"); }
    SECTION("a discard") { ok("_ = r + 1;"); }
    SECTION("an if used as a value") { ok("let v = if (r > 0) r else 0; _ = v;"); }
}

TEST_CASE("A @[discardable] callee may be dropped with no diagnostic") {
    const auto ok{[](std::string_view body) {
        auto [ctx, idx]{helpers::resolve(body)};
        helpers::check_errors<sema::diagnostics>(ctx->root_mod);
    }};

    SECTION("direct call") {
        ok(R"(
            @[discardable] const log = fn(n: i32): i32 { return n; };
            pub const main = fn(): i32 { log(3); return 0; };
        )");
    }
    SECTION("attribute list on its own line") {
        ok(R"(
            @[discardable,]
            const log = fn(n: i32): i32 { return n; };
            pub const main = fn(): i32 { log(3); return 0; };
        )");
    }
    SECTION("through a direct alias") {
        ok(R"(
            @[discardable] const log = fn(n: i32): i32 { return n; };
            const note = log;
            pub const main = fn(): i32 { note(3); return 0; };
        )");
    }
    SECTION("on the function literal itself") {
        ok(R"(
            const log = @[discardable] fn(n: i32): i32 { return n; };
            pub const main = fn(): i32 { log(3); return 0; };
        )");
    }
    SECTION("on an extern function declaration") {
        ok(R"(
            @[discardable] extern const abs: extern fn(n: i32): i32;
            pub const main = fn(): i32 { abs(3); return 0; };
        )");
    }
    SECTION("method call on struct") {
        ok(R"(
            const S = struct {
                @[discardable] pub const close = fn(&self): i32 { return 0; };
            };
            pub const main = fn(): i32 {
                let s: S = .{};
                s.close();
                return 0;
            };
        )");
    }
}

TEST_CASE("@[discardable] on an interface member covers its implementations") {
    constexpr std::string_view shapes{R"(
        const Closer = interface {
            @[discardable] pub const close = fn(&self): i32;
            pub const size = fn(&self): i32;
        };
        const File = struct { fd: i32 };
        impl Closer for File {
            pub const close = fn(&self): i32 { return self.fd; };
            pub const size = fn(&self): i32 { return 0; };
        }
    )"};
    const auto                 with_main{[&](std::string_view body) {
        return fmt::format("{}\npub const main = fn(): i32 {{ {} return 0; }};", shapes, body);
    }};

    SECTION("through a dyn receiver") {
        auto [ctx, idx]{helpers::resolve(with_main(R"(
            const f: File = .{ .fd = 1 };
            const c: &dyn Closer = &f;
            c.close();
        )"))};
        helpers::check_errors<sema::diagnostics>(ctx->root_mod);
    }
    SECTION("on the concrete implementation") {
        auto [ctx, idx]{helpers::resolve(with_main(R"(
            const f: File = .{ .fd = 1 };
            f.close();
        )"))};
        helpers::check_errors<sema::diagnostics>(ctx->root_mod);
    }
    SECTION("through an impl-bound parameter") {
        auto [ctx, idx]{helpers::resolve(fmt::format(R"({}
            const shut = fn(c: impl Closer): void {{ c.close(); }};
            pub const main = fn(): i32 {{
                let f: File = .{{ .fd = 1 }};
                shut(&f);
                return 0;
            }};
        )",
                                                     shapes))};
        helpers::check_errors<sema::diagnostics>(ctx->root_mod);
    }
    SECTION("a member without the attribute stays must-use") {
        CHECK(has_error(with_main(R"(
            const f: File = .{ .fd = 1 };
            const c: &dyn Closer = &f;
            c.size();
        )"),
                        sema::error::UNUSED_RESULT));
    }
    SECTION("a function-only attribute is rejected on an interface member") {
        CHECK(has_error(R"(
            const I = interface { @[inline(.always)] const f = fn(&self): i32; };
        )",
                        sema::error::ILLEGAL_ATTRIBUTE));
    }
}

TEST_CASE("@[discardable] is rejected where it cannot apply") {
    SECTION("on a non-function declaration") {
        CHECK(has_error("@[discardable] const X: i32 = 3;", sema::error::ILLEGAL_ATTRIBUTE));
    }
    SECTION("on a function that returns void") {
        CHECK(has_error("@[discardable] const f = fn(): void {};", sema::error::ILLEGAL_ATTRIBUTE));
    }
    SECTION("on a void function literal") {
        CHECK(has_error("const f = @[discardable] fn(): void {};", sema::error::ILLEGAL_ATTRIBUTE));
    }
}

TEST_CASE("@[discardable(<comptime bool>)] gates the discard behavior on the condition") {
    const auto ok{[](std::string_view body) {
        auto [ctx, idx]{helpers::resolve(body)};
        helpers::check_errors<sema::diagnostics>(ctx->root_mod);
    }};

    SECTION("a true condition allows the drop") {
        ok(R"(
            const ON = true;
            @[discardable(ON)] const log = fn(n: i32): i32 { return n; };
            pub const main = fn(): i32 { log(3); return 0; };
        )");
    }
    SECTION("a false condition keeps the must-use error") {
        CHECK(has_error(R"(
            const OFF = false;
            @[discardable(OFF)] const log = fn(n: i32): i32 { return n; };
            pub const main = fn(): i32 { log(3); return 0; };
        )",
                        sema::error::UNUSED_RESULT));
    }
    SECTION("a false condition on a function literal keeps the must-use error") {
        CHECK(has_error(R"(
            const log = @[discardable(false)] fn(n: i32): i32 { return n; };
            pub const main = fn(): i32 { log(3); return 0; };
        )",
                        sema::error::UNUSED_RESULT));
    }
    SECTION("a non-boolean condition is an error") {
        CHECK(has_error("@[discardable(1 + 1)] const f = fn(): i32 { return 0; };",
                        sema::error::ILLEGAL_ATTRIBUTE));
    }
}

} // namespace ghoti::tests
