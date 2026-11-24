#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fmt/format.h>
#include <stdx/utility.hh>

#include "compiler/gir/function.hh"
#include "compiler/module/memory_loader.hh"
#include "compiler/sema/error.hh"
#include "helpers/uir_diff.hh"
#include "helpers/uir_run.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] auto diff_source(std::string_view                  text,
                               const helpers::pipeline_settings& settings = {})
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

constexpr helpers::pipeline_settings TESTING{.testing = true};

// Whether every diagnostic is `code`, and there is at least one
[[nodiscard]] auto only(const helpers::uir_run& run, sema::error code) -> bool {
    const auto codes{run.codes()};
    return !codes.empty() && std::ranges::all_of(codes, [&](sema::error c) { return c == code; });
}

} // namespace

TEST_CASE("A dropped value is an error unless it is void or a discardable call's", "[uir]") {
    constexpr std::string_view HEADER{R"(
        const make = fn(): i32 { return 7; };
        @[discardable] const log = fn(n: i32): i32 { return n; };
        const noop = fn(): void {};
    )"};
    const auto                 with_body{[&](std::string_view body) {
        return fmt::format(
            "{}pub const f = fn(r: i32): void {{ let mut m = r; {} _ = m; }};", HEADER, body);
    }};

    SECTION("dropped") {
        const auto body{GENERATE(std::string_view{"make();"},
                                 std::string_view{"m + 1;"},
                                 std::string_view{"m;"},
                                 std::string_view{"5;"},
                                 std::string_view{"if (m > 0) make();"},
                                 std::string_view{"match (m) { 0 => m + 1, _ => m = 2, };"})};
        INFO(body);
        const auto run{helpers::uir_rejected(with_body(body))};
        CHECK(only(run, sema::error::UNUSED_RESULT));
    }

    SECTION("used or nothing to use") {
        const auto body{GENERATE(std::string_view{"log(1);"},
                                 std::string_view{"_ = make();"},
                                 std::string_view{"noop();"},
                                 std::string_view{"m = 1; m -= 1;"},
                                 std::string_view{"if (m > 0) m -= 1 else m += 1;"},
                                 std::string_view{"match (m) { 0 => { noop(); }, _ => noop(), }"},
                                 std::string_view{"@assert(true);"})};
        INFO(body);
        DISCARD(helpers::uir_accepted(with_body(body)));
    }
}

TEST_CASE("A match statement's arms need no common type", "[uir]") {
    const auto diff{diff_source(R"(
        const noop = fn(): void {};
        test "statement" {
            let x: i32 = 1;
            match (x) {
                1 => { noop(); },
                _ => @expect(false, "unreachable"),
            }
        }
    )",
                                TESTING)};
    INFO(diff.detail);
    CHECK(agrees(diff));
}

TEST_CASE("A top-level statement is checked but never run", "[uir]") {
    const auto source{
        GENERATE(std::string_view{"_ = 1 + 1;"},
                 std::string_view{"const add = fn(a: i32): i32 { return a; }; _ = add;"},
                 std::string_view{R"(
        const twice = fn(a: i32): i32 { return a * 2; };
        _ = twice(3) + 1;
    )"})};
    INFO(source);
    const auto diff{diff_source(source)};
    INFO(diff.detail);
    CHECK(agrees(diff));
}

TEST_CASE("`@export` gives a function its symbol, and each later one an alias", "[uir]") {
    const auto  run{helpers::uir_accepted(R"(
        const twice = fn(x: i32): i32 { return x * 2; };
        @export(twice, .{ .name = "rt_twice", .linkage = .weak });
        comptime { @export(twice, .{ .name = "rt_twice2" }); }
    )")};
    const auto& functions{run.out().gir->get_functions()};
    const auto  twice{
        std::ranges::find_if(functions, [](const auto& fn) { return fn->get_name() == "twice"; })};
    REQUIRE(twice != functions.end());
    CHECK((*twice)->get_linkage() == gir::linkage::EXPORT);
    CHECK((*twice)->get_link_name() == "rt_twice");
    CHECK((*twice)->get_is_weak());
    REQUIRE((*twice)->get_aliases().size() == 1);
    CHECK((*twice)->get_aliases().front().name == "rt_twice2");
}

TEST_CASE("`@export` rejects a symbol exported twice and what isn't a function", "[uir]") {
    SECTION("twice") {
        const auto run{helpers::uir_rejected(R"(
            const a = fn(): i32 { return 1; };
            const b = fn(): i32 { return 2; };
            @export(a, .{ .name = "same" });
            @export(b, .{ .name = "same" });
        )")};
        CHECK(run.messages().front().starts_with("The symbol 'same' is already exported at"));
    }

    SECTION("not a function") {
        const auto run{helpers::uir_rejected(R"(
            const n: i32 = 1;
            @export(n, .{ .name = "n" });
        )")};
        CHECK(only(run, sema::error::TYPE_MISMATCH));
    }
}

TEST_CASE("A failed compile-time `@expect` reports and goes on, `@require` stops", "[uir]") {
    SECTION("expect") {
        const helpers::uir_run run{R"(
            test "t" {
                comptime {
                    @expect(1 == 2);
                    @expect(false, "second");
                }
            }
        )",
                                   TESTING};
        CHECK(run.codes() == std::vector{sema::error::STATIC_ASSERTION_FAILED,
                                         sema::error::STATIC_ASSERTION_FAILED});
        CHECK(run.messages() ==
              std::vector<std::string>{"Compile-time expectation failed", "second"});
    }

    SECTION("require") {
        const helpers::uir_run run{R"(
            test "t" {
                comptime {
                    @require(1 == 2, "stop");
                    @expect(false);
                }
            }
        )",
                                   TESTING};
        CHECK(run.messages() == std::vector<std::string>{"stop"});
    }
}

TEST_CASE("A test build's tests, skips and source locations", "[uir]") {
    const auto source{GENERATE(std::string_view{R"(
        @[testing] const check = fn(x: i32): void { @expect(x == 2); };
        const add = fn(a: i32, b: i32): i32 { return a + b; };
        test "adds" {
            @expect(add(1, 1) == 2);
            check(add(1, 1));
        }
        test { @require(true); }
    )"},
                               std::string_view{R"(
        test "skipped" { @skip("not ready"); @expect(false); }
        test "where" { const at = @src(); @expect(at.line == 3); _ = @src(); }
    )"},
                               std::string_view{R"(
        @[testing] const later = fn(): void { @skip(); };
        test "helper skips" { later(); @expect(true); }
    )"})};
    INFO(source);
    const auto diff{diff_source(source, TESTING)};
    INFO(diff.detail);
    CHECK(agrees(diff));
}

TEST_CASE("A call to a `noreturn` function ends its path", "[uir]") {
    const auto diff{diff_source(R"(
        const die = fn(code: i32): noreturn { _ = code; @panic("x"); };
        pub const f = fn(x: i32): i32 {
            if (x > 3) {
                die(x);
            }
            return x;
        };
    )")};
    INFO(diff.detail);
    CHECK(agrees(diff));
}

TEST_CASE("A module-level `comptime let mut` is compile-time memory", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        comptime let mut n = 1;
        comptime let mut m: i32 = 2;
        comptime { n += 4; m *= 3; }
        const total = n + m;
    )")};
    CHECK(run.int_value("total") == 11);
}

TEST_CASE("An aligned local gets an aligned slot", "[uir]") {
    const auto diff{diff_source(R"(
        pub const f = fn(comptime T: type): usize {
            @[align(if (@sizeOf(T) > 4) 64 else 32)]
            let mut buf: [4]mut u8 = undefined;
            buf[0] = 1;
            @[align(16)] let fixed: i32 = 5;
            return @intFromPtr(^buf) + @intCast(usize, fixed);
        };
        pub const g = fn(): usize { return f(i32) + f(i64); };
    )")};
    INFO(diff.detail);
    CHECK(agrees(diff));
}

TEST_CASE("An extern declaration takes its attributes", "[uir]") {
    const auto diff{diff_source(R"(
        @[visibility(.hidden)] extern("c") const imported: fn(): i32;
        @[discardable] extern("c") const logged: fn(): i32;
        pub const use = fn(): i32 { logged(); return imported(); };
    )",
                                {.target_opts = {.triple_str = "x86_64-unknown-linux-gnu"}})};
    INFO(diff.detail);
    CHECK(agrees(diff));
}

TEST_CASE("A compile-time call binds a pack parameter's values", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const count = fn(c...): usize { return c.len; };
        const second = fn(c...): auto { return c[1]; };
        const n = count(1, 2, 3);
        const s = second(4, 5, 6);
    )")};
    CHECK(run.int_value("n") == 3);
    CHECK(run.int_value("s") == 5);
}

TEST_CASE("An impl gives a builtin type methods", "[uir]") {
    const auto diff{diff_source(R"(
        const Doubled = interface { const doubled = fn(&self): i32; };
        impl Doubled for i32 {
            pub const doubled = fn(&self): i32 { return self * 2; };
        };
        pub const f = fn(x: i32): i32 { return x.doubled(); };
    )")};
    INFO(diff.detail);
    CHECK(agrees(diff));
}

} // namespace ghoti::tests
