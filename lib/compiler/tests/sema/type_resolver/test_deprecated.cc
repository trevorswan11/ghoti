#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"
#include "support/diagnostic.hh"

namespace ghoti::tests {

namespace {

using helpers::mock_file;

// The warning messages resolving `source` reported, after checking it raised no errors
[[nodiscard]] auto warnings_of(std::string_view         source,
                               sema::deprecation_policy policy = sema::deprecation_policy::WARN,
                               const std::vector<mock_file>& imports = {})
    -> std::vector<std::string> {
    auto [ctx, idx]{helpers::collect(source, imports)};
    ctx->analyzer.set_deprecation_policy(policy);
    ctx->analyzer.resolve_types(ctx->root_mod);
    helpers::check_errors<sema::diagnostics>(ctx->root_mod);

    std::vector<std::string> messages;
    for (const auto& warning : ctx->root_mod.warnings) {
        CHECK(warning.get_error() == sema::error::DEPRECATED_USE);
        CHECK(warning.to_formattable().level == diagnostic_level::WARNING);
        messages.emplace_back(warning.get_message().value_or(""));
    }
    return messages;
}

} // namespace

TEST_CASE("Using a @[deprecated] declaration warns once per use site") {
    const auto warnings{warnings_of(R"(
        @[deprecated("use `newer`")]
        const older := fn(x: i32): i32 { return x; };
        pub const main := fn(): i32 { return older(1) + older(2); };
    )")};
    helpers::check_errors_against<std::string>(
        warnings, "'older' is deprecated: use `newer`", "'older' is deprecated: use `newer`");
}

TEST_CASE("A deprecated item may use deprecated names without warning") {
    CHECK(warnings_of(R"(
        @[deprecated] const older := fn(): i32 { return 1; };
        @[deprecated] const wrapper := fn(): i32 { return older(); };
        @[deprecated] const generic := fn(T: type, x: T): i32 { return older(); };
        @[deprecated] const Legacy := struct {
            pub const make := fn(): i32 { return older(); };
        };
    )")
              .empty());
}

TEST_CASE("A deprecated generic's monomorphs stay quiet; only the call site warns") {
    const auto warnings{warnings_of(R"(
        @[deprecated] const older := fn(): i32 { return 1; };
        @[deprecated] const generic := fn(T: type, x: T): i32 { return older(); };
        pub const main := fn(): i32 { return generic(i32, 1) + generic(u8, 2); };
    )")};
    helpers::check_errors_against<std::string>(
        warnings, "'generic' is deprecated", "'generic' is deprecated");
}

TEST_CASE("Deprecated fields, types and statics warn where they are named") {
    const auto warnings{warnings_of(R"(
        @[deprecated] const Old := struct { a: i32 };
        const P := struct {
            @[deprecated("use y")] x: i32,
            y: i32,
            @[deprecated] pub const ORIGIN: i32 = 0;
        };
        pub const main := fn(): i32 {
            let o: Old = .{ .a = 1 };
            let p: P = .{ .x = 1, .y = 2 };
            return p.x + P.ORIGIN + o.a;
        };
    )")};
    helpers::check_errors_against<std::string>(warnings,
                                               "'Old' is deprecated",
                                               "'x' is deprecated: use y",
                                               "'x' is deprecated: use y",
                                               "'ORIGIN' is deprecated");
}

TEST_CASE("Initializing a deprecated field warns at the initializer") {
    const auto warnings{warnings_of(R"(
        const P := struct { @[deprecated("use y")] x: i32 = 0, y: i32 };
        const U := union { @[deprecated] legacy: i32, current: i32 };
        pub const main := fn(): i32 {
            let a: P = .{ .x = 1, .y = 2 };
            let b := P{ .x = 3, .y = 4 };
            let c: P = .{ .y = 5 };
            let u: U = .{ .legacy = 6 };
            return a.y + b.y + c.y;
        };
    )")};
    helpers::check_errors_against<std::string>(
        warnings, "'x' is deprecated: use y", "'x' is deprecated: use y", "'legacy' is deprecated");
}

TEST_CASE("Deprecated uses inside instantiations warn once per site") {
    const auto warnings{warnings_of(R"(
        @[deprecated("use `fresh`")] const stale := fn(): i32 { return 1; };
        const Boxed := fn(T: type): type {
            return struct { @[deprecated("use value")] old: T = 0, value: T };
        };
        const generic := fn(T: type, x: T): i32 { return stale(); };
        const build := fn(T: type): Boxed(T) { return .{ .old = 1, .value = 2 }; };
        pub const main := fn(): i32 {
            let a := build(i32);
            let b := build(i64);
            return generic(i32, 1) + generic(u8, 2) + a.value + @intCast(i32, b.value);
        };
    )")};
    helpers::check_errors_against<std::string>(
        warnings, "'old' is deprecated: use value", "'stale' is deprecated: use `fresh`");
}

TEST_CASE("A deprecated declaration in another module warns at the importing site") {
    const auto warnings{warnings_of(
        R"(
            import "lib.gh" as lib;
            pub const main := fn(): i32 { return lib.old(); };
        )",
        sema::deprecation_policy::WARN,
        helpers::make_vector<mock_file>(mock_file{
            .path   = "lib.gh",
            .source = R"(@[deprecated("gone soon")] pub const old := fn(): i32 { return 0; };)",
        }))};
    helpers::check_errors_against<std::string>(warnings, "'old' is deprecated: gone soon");
}

TEST_CASE("--deprecated=ignore and --deprecated=error change what a use reports") {
    constexpr std::string_view source{R"(
        @[deprecated] const older := fn(): i32 { return 1; };
        pub const main := fn(): i32 { return older(); };
    )"};
    CHECK(warnings_of(source, sema::deprecation_policy::ALLOW).empty());

    auto [ctx, idx]{helpers::collect(source)};
    ctx->analyzer.set_deprecation_policy(sema::deprecation_policy::DENY);
    ctx->analyzer.resolve_types(ctx->root_mod);
    const auto& diags{UNWRAP(ctx->root_mod.diagnostics.as_opt<sema::diagnostics>())};
    CHECK(std::ranges::any_of(
        diags, [](const auto& d) { return d.get_error() == sema::error::DEPRECATED_USE; }));
    CHECK(ctx->root_mod.warnings.empty());
}

TEST_CASE("A deprecation message must be a string literal") {
    CHECK(helpers::raised("@[deprecated(1)] const older := fn(): i32 { return 1; };",
                          sema::error::ILLEGAL_ATTRIBUTE));
    CHECK(helpers::raised(R"(@[deprecated("fine")] const older := fn(): i32 { return 1; };)",
                          sema::error::ILLEGAL_ATTRIBUTE) == false);
}

} // namespace ghoti::tests
