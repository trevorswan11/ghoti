#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/utility.hh>

#include "compiler/module/memory_loader.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "helpers/uir_diff.hh"
#include "helpers/uir_run.hh"
#include "support/diagnostic.hh"

namespace ghoti::tests {

namespace {

using sema::error;

[[nodiscard]] auto diff_source(std::string_view text) -> helpers::uir_diff {
    mod::memory_loader loader;
    loader.add("main.gh", std::string{text});
    return helpers::diff_pipelines(loader, "main.gh", {}, {});
}

auto check_match(std::string_view text) -> void {
    const auto diff{diff_source(text)};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

// The deprecation warnings a program reports in sorted order, after checking it has no errors
[[nodiscard]] auto warnings_of(std::string_view         source,
                               sema::deprecation_policy policy = sema::deprecation_policy::WARN)
    -> std::vector<std::string> {
    const helpers::uir_run run{source, {.deprecated_policy = policy}};
    if (run.out().stopped_at) { FAIL("UIR Sema stopped at " << run.out().stopped_at->what); }
    INFO(fmt::format("{}", run.out().diagnostics));
    REQUIRE(run.out().succeeded());
    for (const auto& w : run.out().warnings) {
        CHECK(w.get_error() == error::DEPRECATED_USE);
        CHECK(w.to_formattable().level == diagnostic_level::WARNING);
    }
    auto found{run.warnings()};
    std::ranges::sort(found);
    return found;
}

// The `@[testing]` errors a test build of `source` reports
[[nodiscard]] auto testing_errors(std::string_view source) -> std::vector<error> {
    const helpers::uir_run run{source, {.testing = true}};
    if (run.out().stopped_at) { FAIL("UIR Sema stopped at " << run.out().stopped_at->what); }
    return run.codes();
}

} // namespace

TEST_CASE("An array a slice initializer names is sliced where it lives", "[uir]") {
    check_match(R"(
        let mut three: [3]i32 = .{ 1, 2, 3 };
        let mut five: [5]mut i32 = .{ 10, 20, 30, 40, 50 };
        pub const main = fn(): i32 {
            let view: []i32 = three;
            let writable: []mut i32 = five;
            writable[0] = 7;
            return view[1] + five[0];
        };
    )");
}

TEST_CASE("An untyped literal in a pack takes its own default type, not the call's", "[uir]") {
    DISCARD(helpers::uir_accepted(R"(
        const second = fn(args...): type { return @TypeOf(args[1]); };
        const count = fn(args...): u32 { return args[0] + @intCast(u32, args[1]); };
        comptime { @assert(second(@as(u32, 1), 3) == i32); }
        pub const main = fn(): u32 {
            let x: u32 = 1;
            return count(x, 3);
        };
    )"));

    const auto run{helpers::uir_rejected(R"(
        const pick = fn(args...): u32 { return args[1]; };
        pub const main = fn(): u32 {
            let x: u32 = 1;
            return pick(x, 3);
        };
    )")};
    CHECK(run.codes() == std::vector{error::RETURN_TYPE_MISMATCH});
}

TEST_CASE("A deprecated declaration warns once at each site that names it", "[uir]") {
    CHECK(warnings_of(R"(
        @[deprecated("use `newer`")]
        const older = fn(x: i32): i32 { return x; };
        pub const main = fn(): i32 { return older(1) + older(2); };
    )") == std::vector<std::string>{"'older' is deprecated: use `newer`",
                                    "'older' is deprecated: use `newer`"});
}

TEST_CASE("Deprecated code names deprecated declarations quietly", "[uir]") {
    CHECK(warnings_of(R"(
        @[deprecated] const older = fn(): i32 { return 1; };
        @[deprecated] const wrapper = fn(): i32 { return older(); };
        @[deprecated] const generic = fn(comptime T: type, x: T): i32 { return older(); };
        @[deprecated] const Legacy = struct {
            pub const make = fn(): i32 { return older(); };
        };
        pub const main = fn(): i32 { return 0; };
    )")
              .empty());

    // A deprecated generic's instances stay quiet, and only its call sites warn
    CHECK(warnings_of(R"(
        @[deprecated] const older = fn(): i32 { return 1; };
        @[deprecated] const generic = fn(comptime T: type, x: T): i32 { return older(); };
        pub const main = fn(): i32 { return generic(i32, 1) + generic(u8, 2); };
    )") == std::vector<std::string>{"'generic' is deprecated", "'generic' is deprecated"});
}

TEST_CASE("Deprecated fields, types and members warn where they are named", "[uir]") {
    CHECK(warnings_of(R"(
        @[deprecated] const Old = struct { a: i32 };
        const P = struct {
            @[deprecated("use y")] x: i32,
            y: i32,
            @[deprecated] pub const ORIGIN: i32 = 0;
        };
        const U = union(enum) { @[deprecated] legacy: i32, current: i32 };
        pub const main = fn(): i32 {
            let o: Old = .{ .a = 1 };
            let p: P = .{ .x = 1, .y = 2 };
            let u: U = .{ .legacy = 6 };
            return p.x + P.ORIGIN + o.a;
        };
    )") == std::vector<std::string>{"'ORIGIN' is deprecated",
                                    "'Old' is deprecated",
                                    "'legacy' is deprecated",
                                    "'x' is deprecated: use y",
                                    "'x' is deprecated: use y"});
}

TEST_CASE("A deprecated declaration in another module warns where it is imported", "[uir]") {
    mod::memory_loader loader;
    loader.add("main.gh", R"(
        import "lib.gh" as lib;
        pub const main = fn(): i32 { return lib.old(); };
    )");
    loader.add("lib.gh", R"(@[deprecated("gone soon")] pub const old = fn(): i32 { return 0; };)");
    const auto run{helpers::run_uir_sema_files(loader, "main.gh", {})};
    REQUIRE(run->out->succeeded());
    REQUIRE(run->out->warnings.size() == 1);
    CHECK((*run->out->warnings.begin()).get_message() == "'old' is deprecated: gone soon");
}

TEST_CASE("The deprecation policy silences a deprecated use or makes it an error", "[uir]") {
    constexpr std::string_view SOURCE{R"(
        @[deprecated] const older = fn(): i32 { return 1; };
        pub const main = fn(): i32 { return older(); };
    )"};
    CHECK(warnings_of(SOURCE, sema::deprecation_policy::ALLOW).empty());

    const helpers::uir_run denied{SOURCE, {.deprecated_policy = sema::deprecation_policy::DENY}};
    CHECK(denied.codes() == std::vector{error::DEPRECATED_USE});
    CHECK(denied.out().warnings.empty());
}

TEST_CASE("A `@[testing]` function is called directly, by a test or another one", "[uir]") {
    constexpr std::string_view HELPER{"@[testing] const helper = fn(): void { @expect(true); };\n"};
    const auto                 errors{
        [&](std::string_view rest) { return testing_errors(fmt::format("{}{}", HELPER, rest)); }};
    CHECK(errors(R"(test { helper(); })").empty());
    CHECK(errors(R"(
        @[testing] const outer = fn(): void { helper(); };
        test { outer(); }
    )")
              .empty());
    CHECK(errors(R"(
        const plain = fn(): void { helper(); };
        test { plain(); }
    )") == std::vector{error::TESTING_FN_OUTSIDE_TEST});
    CHECK(errors(R"(test { let f = helper; f(); })") ==
          std::vector{error::TESTING_FN_OUTSIDE_TEST});
    CHECK(errors(R"(test { let c = fn(): void { helper(); }; c(); })") ==
          std::vector{error::TESTING_FN_OUTSIDE_TEST});
}

} // namespace ghoti::tests
