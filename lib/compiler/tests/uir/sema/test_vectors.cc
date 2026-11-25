#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "compiler/module/memory_loader.hh"
#include "compiler/sema/error.hh"
#include "helpers/uir_diff.hh"
#include "helpers/uir_run.hh"

namespace ghoti::tests {

namespace {

using sema::error;

[[nodiscard]] auto diff_source(std::string_view text) -> helpers::uir_diff {
    mod::memory_loader loader;
    loader.add("main.gh", std::string{text});
    return helpers::diff_pipelines(loader, "main.gh", {}, {});
}

} // namespace

TEST_CASE("A runtime vector operator splats a known scalar", "[uir]") {
    const auto diff{diff_source(R"(
        pub const scale = fn(v: @Vector(4, i32)): @Vector(4, i32) { return v * 2 + 1; };
        pub const pick = fn(c: @Vector(4, bool), v: @Vector(4, f32)): @Vector(4, f32) {
            return @select(c, v, 0.5);
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A known vector binding is built where it is bound and folds where it is used", "[uir]") {
    const auto diff{diff_source(R"(
        const first = fn(v: auto): i32 { return v[0]; };
        pub const f = fn(): i32 {
            let a: @Vector(4, i32) = .{ 1, 2, 3, 4 };
            const b: @Vector(4, i32) = .{ 5, 6, 7, 8 };
            let c = a + b;
            return first(a) + c[3] + (-b)[1];
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A vector's lanes are written through the whole vector", "[uir]") {
    const auto diff{diff_source(R"(
        pub const f = fn(i: usize): i32 {
            let mut a: @Vector(4, i32) = .{ 1, 2, 3, 4 };
            a[2] = 30;
            a[0] += 9;
            a[i] *= 10;
            return a[0] + a[i];
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A vector lane has no address", "[uir]") {
    const auto run{helpers::uir_rejected(R"(
        pub const f = fn(): i32 {
            let mut a: @Vector(4, i32) = .{ 1, 2, 3, 4 };
            let p = ^a[1];
            return *p;
        };
    )")};
    CHECK(run.codes() == std::vector{error::ILLEGAL_VECTOR_LANE_ADDRESS});
}

TEST_CASE("Unary operators, `and` and `or` apply lane by lane", "[uir]") {
    const auto diff{diff_source(R"(
        pub const f = fn(a: @Vector(4, i32), b: @Vector(4, u8)): @Vector(4, bool) {
            const n = -a;
            const m = ~a;
            const w = -%b;
            return (n > m) and !(w == b) or a == .{ 1, 2, 3, 4 };
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("Bitwise operators on known bool lanes fold as bitwise operators", "[uir]") {
    // The old evaluator doesn't fold these, so only UIR's result is checked
    const auto run{helpers::uir_accepted(R"(
        const a: @Vector(2, bool) = .{ true, false };
        const t: @Vector(2, bool) = .{ true, true };
        const u: @Vector(2, bool) = .{ false, true };
        const b = a & t;
        const c = a | u;
        const x = a ^ t;
        pub const ok: i32 = if (b[0] and !b[1] and c[0] and c[1] and !x[0] and x[1]) 1 else 0;
    )")};
    CHECK(run.int_value("ok") == 1);
}

TEST_CASE("Numeric casts convert a vector lane by lane", "[uir]") {
    const auto diff{diff_source(R"(
        pub const f = fn(v: @Vector(4, i32), x: @Vector(4, f32)): @Vector(4, u8) {
            const t: @Vector(4, u8) = @truncate(v);
            const i: @Vector(4, u8) = @intFromFloat(x);
            const n: @Vector(4, u8) = @intCast(v);
            return t + i + n;
        };
        pub const widen = fn(v: @Vector(2, f32)): @Vector(2, f64) { return @floatCast(v); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A vector cast needs as many lanes of a kind it converts", "[uir]") {
    const auto lanes{helpers::uir_rejected(R"(
        pub const f = fn(v: @Vector(4, i32)): @Vector(2, u8) { return @truncate(v); };
    )")};
    CHECK(lanes.codes() == std::vector{error::TYPE_MISMATCH});
    CHECK(lanes.messages().front() ==
          "'@truncate' converts a vector to a vector of as many lanes; found '@Vector(4, i32)' to "
          "'@Vector(2, u8)'");

    const auto kind{helpers::uir_rejected(R"(
        pub const f = fn(v: @Vector(4, f32)): @Vector(4, u8) { return @intCast(v); };
    )")};
    CHECK(kind.codes() == std::vector{error::TYPE_MISMATCH});
    CHECK(kind.messages().front() == "'@intCast' can't convert 'f32' lanes to 'u8' lanes");
}

TEST_CASE("`@shuffle` picks lanes of either operand by a known mask", "[uir]") {
    const auto diff{diff_source(R"(
        pub const f = fn(a: @Vector(2, i32), b: @Vector(4, i32)): @Vector(4, i32) {
            return @shuffle(a, b, .{ 1, -4, -1, 0 });
        };
        pub const g = fn(a: @Vector(4, i32)): @Vector(2, i32) {
            return @shuffle(a, undefined, .{ 3, 0 });
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("`@shuffle` rejects a mask entry past its operand", "[uir]") {
    const auto run{helpers::uir_rejected(R"(
        pub const f = fn(a: @Vector(2, i32)): @Vector(2, i32) {
            return @shuffle(a, a, .{ 0, 5 });
        };
    )")};
    CHECK(run.codes() == std::vector{error::TYPE_MISMATCH});
    CHECK(run.messages().front() ==
          "'@shuffle': mask entry 1 (5) selects lane 5 of '@Vector(2, i32)', which has 2");
}

} // namespace ghoti::tests
