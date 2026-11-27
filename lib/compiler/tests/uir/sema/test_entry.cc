#include <string>
#include <string_view>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "compiler/module/memory_loader.hh"
#include "helpers/uir_diff.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] auto diff_source(std::string_view text, const helpers::pipeline_settings& settings)
    -> helpers::uir_diff {
    mod::memory_loader loader;
    loader.add("main.gh", std::string{text});
    auto diff{helpers::diff_pipelines(loader, "main.gh", {}, settings)};
    helpers::report_uir_diff(diff);
    return diff;
}

// The same GIR, or the same effects with values computed in another place
[[nodiscard]] auto agrees(const helpers::uir_diff& diff) -> bool {
    return diff.status == helpers::uir_diff_status::MATCH ||
           diff.status == helpers::uir_diff_status::EVAL_ORDER ||
           diff.status == helpers::uir_diff_status::FOLD_PLACEMENT;
}

constexpr helpers::pipeline_settings EXECUTABLE{.entry = true};
constexpr helpers::pipeline_settings TESTS{.testing = true, .entry = true};

// Each OS's entry path, whatever the host
[[nodiscard]] auto on_each_os(helpers::pipeline_settings settings) -> helpers::pipeline_settings {
    settings.target_opts.triple_str = GENERATE(std::string{"x86_64-unknown-linux-gnu"},
                                               std::string{"x86_64-pc-windows-msvc"},
                                               std::string{"aarch64-apple-darwin"});
    return settings;
}

} // namespace

TEST_CASE("An executable's entry code calls `main`, whatever it returns", "[uir]") {
    const auto source{GENERATE(std::string_view{"pub const main = fn(): i32 { return 3; };"},
                               std::string_view{"pub const main = fn(): void {};"},
                               std::string_view{R"(
        pub const main = fn(args: [][:0]u8): i32 { return @intCast(i32, args.len); };
    )"},
                               std::string_view{"pub const main = fn(): noreturn { @trap(); };"})};
    const auto settings{on_each_os(EXECUTABLE)};
    const auto diff{diff_source(source, settings)};
    INFO(source);
    INFO(*settings.target_opts.triple_str);
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

TEST_CASE("A function exported as `main` is the entry point", "[uir]") {
    const auto diff{diff_source(R"(
        const start_here = fn(): i32 { return 7; };
        @export(start_here, .{ .name = "main" });
    )",
                                EXECUTABLE)};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

TEST_CASE("A test executable's entry code runs the tests through the runner", "[uir]") {
    const auto source{GENERATE(std::string_view{R"(
        const add = fn(a: i32, b: i32): i32 { return a + b; };
        test "adds" { @expect(add(1, 2) == 3); }
        test { @require(true); }
    )"},
                               std::string_view{R"(
        pub const test_runner = fn(_: [][:0]u8, tests: []builtin.Test): i32 {
            return @intCast(i32, tests.len);
        };
        test "one" {}
    )"})};
    const auto settings{on_each_os(TESTS)};
    const auto diff{diff_source(source, settings)};
    INFO(source);
    INFO(*settings.target_opts.triple_str);
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

} // namespace ghoti::tests
