#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

using helpers::mock_file;

// The deepest module: declares one of every re-exportable thing.
constexpr std::string_view LEAF{R"(
    pub var counter: i32 = 100;
    pub const bump := fn(): void { counter = counter + 1; };
    pub const get := fn(): i32 { return counter; };

    pub const PAGE: usize = 4;

    pub const Point := struct {
        pub x: i32,
        pub y: i32,
        pub const sum := fn(&self): i32 { return self.x + self.y; };
    };

    pub const Tag := enum { red, green, blue };

    pub const Box := fn(T: type): type { return struct { pub val: T }; };
)"};

// Middle module: re-exports the leaf and also aliases one of its symbols.
constexpr std::string_view MID{R"(
    pub import "leaf.gh" as leaf;
    pub using Coord = leaf::Point;
)"};

// Top module: re-exports the middle module.
constexpr std::string_view TOP{R"(
    pub import "mid.gh" as top_mid;
)"};

[[nodiscard]] auto chain_files() {
    return helpers::make_vector<mock_file>(mock_file{"leaf.gh", LEAF, "leaf"},
                                           mock_file{"mid.gh", MID, "mid"},
                                           mock_file{"top.gh", TOP, "top"});
}

} // namespace

TEST_CASE("E2E: a re-exported `pub var` has one storage location across the whole chain") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "top.gh" as top;
            pub const main := fn(): i32 {
                top::top_mid::leaf::bump();
                top::top_mid::leaf::bump();
                return top::top_mid::leaf::get();
            };
        )",
        chain_files())};
    CHECK(exit_code == 102);
}

TEST_CASE("E2E: a re-exported `pub const` folds as a compile-time array dimension") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "top.gh" as top;
            pub const main := fn(): i32 {
                var buf: [top::top_mid::leaf::PAGE]mut i32 = undefined;
                buf[3] = 42;   // valid only if PAGE >= 4
                return buf[3];
            };
        )",
        chain_files())};
    CHECK(exit_code == 42);
}

TEST_CASE("E2E: a re-exported `pub const` folds as a value operand through the chain") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "top.gh" as top;
            pub const main := fn(): i32 {
                return @as(i32, top::top_mid::leaf::PAGE) * 10 + 2;
            };
        )",
        chain_files())};
    CHECK(exit_code == 42);
}

TEST_CASE("E2E: a re-exported `struct` type is constructible with a callable member") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "top.gh" as top;
            pub const main := fn(): i32 {
                const p: top::top_mid::leaf::Point = .{ .x = 30, .y = 12 };
                return p.sum() + p.x - p.x;   // method + direct pub field access
            };
        )",
        chain_files())};
    CHECK(exit_code == 42);
}

TEST_CASE("E2E: a `pub using` alias of a re-exported symbol is itself re-exported") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "top.gh" as top;
            pub const main := fn(): i32 {
                const c: top::top_mid::Coord = .{ .x = 40, .y = 2 };
                return c.sum();
            };
        )",
        chain_files())};
    CHECK(exit_code == 42);
}

TEST_CASE("E2E: a re-exported `enum` matches by variant through the chain") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "top.gh" as top;
            pub const main := fn(): i32 {
                const t: top::top_mid::leaf::Tag = .green;
                return match (t) {
                    .red => 1,
                    .green => 42,
                    .blue => 3,
                };
            };
        )",
        chain_files())};
    CHECK(exit_code == 42);
}

TEST_CASE("E2E: a re-exported generic type constructor instantiates through the chain") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "top.gh" as top;
            pub const main := fn(): i32 {
                const b: top::top_mid::leaf::Box(i32) = .{ .val = 42 };
                return b.val;
            };
        )",
        chain_files())};
    CHECK(exit_code == 42);
}

TEST_CASE("E2E: same-named symbols from two leaves re-exported into one parent stay distinct") {
    constexpr std::string_view a_leaf{R"( pub const kind := fn(): i32 { return 10; }; )"};
    constexpr std::string_view b_leaf{R"( pub const kind := fn(): i32 { return 32; }; )"};
    constexpr std::string_view combined{R"(
        pub import "a_leaf.gh" as a;
        pub import "b_leaf.gh" as b;
    )"};

    const auto exit_code{helpers::compile_and_run(
        R"(
            import "combined.gh" as combined;
            pub const main := fn(): i32 {
                return combined::a::kind() + combined::b::kind();
            };
        )",
        {
            mock_file{"a_leaf.gh", a_leaf, "a_leaf"},
            mock_file{"b_leaf.gh", b_leaf, "b_leaf"},
            mock_file{"combined.gh", combined, "combined"},
        })};
    CHECK(exit_code == 42);
}

} // namespace ghoti::tests
