#include <algorithm>
#include <string_view>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "helpers/uir_diff.hh"

namespace ghoti::tests {

namespace {

constexpr helpers::pipeline_settings LAZY_EXECUTABLE{.entry = true, .check_all = false};
constexpr helpers::pipeline_settings LAZY_TESTS{.testing = true, .entry = true, .check_all = false};
constexpr helpers::pipeline_settings CHECK_ALL_EXECUTABLE{.entry = true};

[[nodiscard]] auto has_function(const helpers::uir_sema_run& run, std::string_view name) -> bool {
    return std::ranges::any_of(run.out->gir->get_functions(),
                               [&](const auto& fn) { return fn->get_name() == name; });
}

} // namespace

TEST_CASE("A lazy build skips the errors of declarations nothing reaches", "[uir]") {
    const auto source{GENERATE(std::string_view{R"(
        const unused = fn(): i32 { return true; };
        pub const main = fn(): i32 { return 0; };
    )"},
                               std::string_view{R"(
        const unused: i32 = true;
        pub const main = fn(): i32 { return 0; };
    )"},
                               std::string_view{R"(
        const S = struct {
            const broken = fn(): i32 { return true; };
        };
        pub const main = fn(): i32 { return 0; };
    )"},
                               std::string_view{R"(
        const Shape = interface { pub const area = fn(&self): i32; };
        const Square = struct { side: i32 };
        impl Shape for Square {
            pub const area = fn(&self): i32 { return true; };
        }
        pub const main = fn(): i32 { return 0; };
    )"})};
    INFO(source);

    const auto lazy{helpers::run_uir_sema(source, LAZY_EXECUTABLE)};
    REQUIRE_FALSE(lazy->out->stopped_at);
    CHECK(lazy->out->succeeded());

    const auto eager{helpers::run_uir_sema(source, CHECK_ALL_EXECUTABLE)};
    REQUIRE_FALSE(eager->out->stopped_at);
    CHECK_FALSE(eager->out->succeeded());
}

TEST_CASE("A lazy build reports the errors of what its roots reach", "[uir]") {
    const auto source{GENERATE(std::string_view{R"(
        const helper = fn(): i32 { return true; };
        const middle = fn(): i32 { return helper(); };
        pub const main = fn(): i32 { return middle(); };
    )"},
                               std::string_view{R"(
        const exported = fn(): i32 { return true; };
        @export(exported, .{ .name = "exported" });
        pub const main = fn(): i32 { return 0; };
    )"},
                               std::string_view{R"(
        const limit: i32 = true;
        pub const main = fn(): i32 { return limit; };
    )"},
                               std::string_view{R"(
        comptime { const bad: i32 = true; }
        pub const main = fn(): i32 { return 0; };
    )"})};
    INFO(source);
    const auto lazy{helpers::run_uir_sema(source, LAZY_EXECUTABLE)};
    REQUIRE_FALSE(lazy->out->stopped_at);
    CHECK_FALSE(lazy->out->succeeded());
}

TEST_CASE("A lazy build emits only the functions its roots reach", "[uir]") {
    const auto run{helpers::run_uir_sema(R"(
        const used = fn(): i32 { return 1; };
        const unused = fn(): i32 { return 2; };
        pub const main = fn(): i32 { return used(); };
    )",
                                         LAZY_EXECUTABLE)};
    REQUIRE(run->out->succeeded());
    CHECK(has_function(*run, "used"));
    CHECK_FALSE(has_function(*run, "unused"));
}

TEST_CASE("A lazy test build roots its tests", "[uir]") {
    const auto passing{helpers::run_uir_sema(R"(
        const unused = fn(): i32 { return true; };
        const twice = fn(x: i32): i32 { return x * 2; };
        test "twice" { @expect(twice(2) == 4); }
    )",
                                             LAZY_TESTS)};
    REQUIRE_FALSE(passing->out->stopped_at);
    CHECK(passing->out->succeeded());
    CHECK(has_function(*passing, "twice"));

    const auto failing{helpers::run_uir_sema(R"(
        const broken = fn(): i32 { return true; };
        test "broken" { @expect(broken() == 1); }
    )",
                                             LAZY_TESTS)};
    REQUIRE_FALSE(failing->out->stopped_at);
    CHECK_FALSE(failing->out->succeeded());
}

} // namespace ghoti::tests
