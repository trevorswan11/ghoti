#include <string_view>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fmt/format.h>

#include "compiler/sema/error.hh"

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`rest[lo..hi]...` forwards only the selected pack elements") {
    const auto [slice, expected]{GENERATE(std::tuple{"rest[1..]", 23U},
                                          std::tuple{"rest[..2]", 12U},
                                          std::tuple{"rest[1..2]", 2U},
                                          std::tuple{"rest[1..=2]", 23U},
                                          std::tuple{"rest[..]", 123U},
                                          std::tuple{"rest[0..3]", 123U},
                                          std::tuple{"rest[2..2]", 0U},
                                          std::tuple{"rest[3..]", 0U})};

    CHECK(helpers::compile_and_run(fmt::format(R"(
            const digits = fn(rest...): i32 {{
                let mut t: i32 = 0;
                unroll for (rest) |v| {{ t = t * 10 + v; }}
                return t;
            }};
            const forward = fn(rest...): i32 {{ return digits({}...); }};
            pub const main = fn(): i32 {{ return forward(1, 2, 3); }};
        )",
                                               slice)) == expected);
}

TEST_CASE("A pack slice bound may be any compile-time constant expression") {
    CHECK(helpers::compile_and_run(R"(
        const SKIP: usize = 1;
        const digits = fn(rest...): i32 {
            let mut t: i32 = 0;
            unroll for (rest) |v| { t = t * 10 + v; }
            return t;
        };
        const forward = fn(rest...): i32 { return digits(rest[SKIP..rest.len - 1]...); };
        pub const main = fn(): i32 { return forward(1, 2, 3, 4); };
    )") == 23);
}

TEST_CASE("A pack slice recurses over the tail of a heterogeneous pack") {
    CHECK(helpers::compile_and_run(R"(
        const sum = fn(rest...): i32 {
            if comptime (rest.len == 0) {
                return 0;
            } else {
                return @as(i32, @intCast(rest[0])) + sum(rest[1..]...);
            }
        };
        pub const main = fn(): i32 {
            return sum(@as(u8, 1), @as(i64, 2), @as(u16, 3), 4);
        };
    )") == 10);
}

TEST_CASE("A pack slice fills a plain function's fixed parameters and mixes with other args") {
    CHECK(helpers::compile_and_run(R"(
        const add3 = fn(a: i32, b: i32, c: i32): i32 { return a + b * 10 + c * 100; };
        const call = fn(rest...): i32 {
            return add3(rest[2..]..., 9, rest[..1]...) + @call(.never_inline, add3, rest[1..]..., 0);
        };
        pub const main = fn(): i32 { return call(1, 2, 3); };
    )") == (3 + 90 + 100) + (2 + 30));
}

TEST_CASE("A closure expands a slice of the pack it captures, and of its own pack") {
    CHECK(helpers::compile_and_run(R"(
        const digits = fn(rest...): i32 {
            let mut t: i32 = 0;
            unroll for (rest) |v| { t = t * 10 + @as(i32, @intCast(v)); }
            return t;
        };
        const outer = fn(rest...): i32 {
            let k: i32 = 100;
            let tail = fn(): i32 { return digits(rest[1..rest.len]...); };
            let own = fn(xs...): i32 { return digits(xs[..2]...) + k; };
            return tail() + own(@as(u8, 7), 8, 9);
        };
        pub const main = fn(): i32 { return outer(1, 2, 3); };
    )") == 23 + 78 + 100);
}

TEST_CASE("A pack slice folds in a compile-time call") {
    CHECK(helpers::compile_and_run(R"(
        const digits = fn(rest...): i32 {
            let mut t: i32 = 0;
            unroll for (rest) |v| { t = t * 10 + v; }
            return t;
        };
        const forward = fn(rest...): i32 { return digits(rest[1..3]...); };
        pub const main = fn(): i32 {
            let n = comptime forward(1, 2, 3, 4);
            return n;
        };
    )") == 23);
}

TEST_CASE("`unroll for` over a pack slice visits only the selected elements") {
    const auto [slice, expected]{GENERATE(std::tuple{"rest[1..]", 23U},
                                          std::tuple{"rest[..2]", 12U},
                                          std::tuple{"rest[1..=1]", 2U},
                                          std::tuple{"rest[..]", 123U},
                                          std::tuple{"rest[1..rest.len]", 23U},
                                          std::tuple{"rest[3..]", 0U})};

    CHECK(helpers::compile_and_run(fmt::format(R"(
            const digits = fn(rest...): i32 {{
                let mut t: i32 = 0;
                unroll for ({}) |v| {{ t = t * 10 + v; }}
                return t;
            }};
            pub const main = fn(): i32 {{ return digits(1, 2, 3); }};
        )",
                                               slice)) == expected);
}

TEST_CASE("`unroll for` zips a pack slice with other drivers and a companion index") {
    CHECK(helpers::compile_and_run(R"(
        const pairs = fn(rest...): i32 {
            let mut t: i32 = 0;
            unroll for (rest[1..], rest[..rest.len - 1], 0..) |next, prev, i| {
                t += (@as(i32, @intCast(next)) - @as(i32, @intCast(prev))) * @as(i32, @intCast(i + 1));
            }
            return t;
        };
        pub const main = fn(): i32 { return pairs(@as(u8, 1), @as(i64, 4), @as(u16, 9)); };
    )") == 3 * 1 + 5 * 2);
}

TEST_CASE("A closure iterates a slice of the pack it captures") {
    CHECK(helpers::compile_and_run(R"(
        const outer = fn(rest...): i32 {
            let mut t: i32 = 0;
            let add_tail = fn(): void { unroll for (rest[1..]) |v| { t += @as(i32, @intCast(v)); } };
            add_tail();
            return t;
        };
        pub const main = fn(): i32 { return outer(100, 2, @as(u8, 3)); };
    )") == 5);
}

TEST_CASE("`unroll for` over a pack slice folds in a compile-time call") {
    CHECK(helpers::compile_and_run(R"(
        const tail_sum = fn(rest...): i32 {
            let mut t: i32 = 0;
            unroll for (rest[1..3]) |v| { t += v; }
            return t;
        };
        pub const main = fn(): i32 {
            let n = comptime tail_sum(1, 20, 30, 4);
            return n;
        };
    )") == 50);
}

TEST_CASE("A pack slice's bounds must be in range and in order") {
    const std::string_view slice{
        GENERATE("rest[1..4]", "rest[3..]", "rest[2..1]", "rest[..=2]", "rest[4..5]")};
    const std::string_view use{GENERATE("_ = sum({}...);", "unroll for ({}) |v| {{ _ = v; }}")};
    CHECK(helpers::raised(fmt::format(R"(
            const sum = fn(rest...): i32 {{ return @intCast(rest.len); }};
            const f = fn(rest...): void {{ {} }};
            const use = fn(): void {{ f(1, 2); }};
        )",
                                      fmt::format(fmt::runtime(use), slice)),
                          sema::error::PACK_INDEX_OUT_OF_RANGE));
}

TEST_CASE("A pack slice's bounds must be compile-time constants") {
    CHECK(helpers::raised(R"(
        const sum = fn(rest...): i32 { return @intCast(rest.len); };
        const f = fn(n: usize, rest...): i32 { return sum(rest[n..]...); };
        const use = fn(): void { _ = f(1, 2, 3); };
    )",
                          sema::error::PACK_INDEX_NOT_CONST));
}

TEST_CASE("An `unroll for` pack slice bound reads its parameter at compile time") {
    constexpr std::string_view tail_sum{R"(
        const tail_sum = fn(n: usize, rest...): i32 {
            let mut t: i32 = 0;
            unroll for (rest[n..]) |v| { t += v; }
            return t;
        };
    )"};
    CHECK(helpers::compile_and_run(fmt::format(
              "{}pub const main = fn(): i32 {{ return tail_sum(1, 2, 3) + tail_sum(0, 4); }};",
              tail_sum)) == 7);
    CHECK(helpers::raised(
        fmt::format("{}const use = fn(): void {{ let mut n: usize = 0; n += 1; _ = tail_sum(n, 2); "
                    "}};",
                    tail_sum),
        sema::error::COMPTIME_EVALUATION_FAILED));
}

TEST_CASE("A pack slice is only usable through `...`") {
    CHECK(helpers::raised(R"(
        const f = fn(rest...): i32 { let s = rest[1..]; return 0; };
        const use = fn(): void { _ = f(1, 2); };
    )",
                          sema::error::MALFORMED_PACK_USE));
}

TEST_CASE("Slicing something other than a pack still cannot be expanded") {
    helpers::expect_compile_error(R"(
        const sum = fn(rest...): i32 { return @intCast(rest.len); };
        const bad = fn(rest...): i32 {
            let arr: [2]i32 = .{1, 2};
            return sum(arr[0..]...);
        };
        pub const main = fn(): i32 { return bad(1, 2); };
    )");
}

} // namespace ghoti::tests
