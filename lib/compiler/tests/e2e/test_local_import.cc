#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view IMPORTED_TESTS{R"(
    test "imported failing" { @require(1 + 1 == 3); }
    test "imported passing" { @expect(true); }
)"};

constexpr std::string_view IMPORTED_FN{R"(
    pub const answer = fn(): i32 { return 42; };
)"};

constexpr std::string_view CYCLE_LIB{R"(
    import "first.gh" as first;
    import "box.gh" as box;
    pub const Box = box.Box;
    pub import "util.gh" as util;
    pub import "meta.gh" as meta;
    pub const k = first.k;
)"};

// Resolving `k`'s initializer resolves `meta`, whose test reaches `lib.Box` mid-resolution
constexpr std::string_view CYCLE_FIRST{R"(
    import "lib.gh" as lib;
    pub const k = lib.meta.k;
)"};

constexpr std::string_view CYCLE_META{R"(
    import "lib.gh" as lib;
    pub const k = 1;
    test "meta" {
        let b: lib.Box(u8) = .{ .v = 1 };
        @expect(b.v == 1);
    }
)"};

// Instantiating `same` from this test resolves `util`, whose `wrap` names `lib.Box`
constexpr std::string_view CYCLE_BOX{R"(
    import "lib.gh" as lib;
    pub const Box = fn(comptime T: type): type {
        return struct {
            pub v: T,
            pub const same = fn(&self, val: auto): bool {
                return lib.util.matches(T, @TypeOf(val));
            };
        };
    };
    test "box" {
        let b: Box(u8) = .{ .v = 1 };
        @expect(!b.same(1));
    }
)"};

constexpr std::string_view CYCLE_UTIL{R"(
    import "lib.gh" as lib;
    pub const wrap = fn(comptime T: type, x: T): lib.Box(T) { return .{ .v = x }; };
    pub const matches = fn(comptime T: type, comptime V: type): bool { return T == V; };
)"};

} // namespace

TEST_CASE("A root test block that only imports a file runs that file's tests") {
    CHECK(helpers::compile_and_run_tests(
              R"(test "bundle" { import "suite.gh" as suite; })",
              {helpers::mock_file{"suite.gh", IMPORTED_TESTS, "suite"}}) != 0);
}

TEST_CASE("An import inside a function body still links the imported module") {
    CHECK(helpers::compile_and_run(
              R"(
            pub const main = fn(): i32 {
                import "dep.gh" as dep;
                return dep.answer();
            };
        )",
              {helpers::mock_file{"dep.gh", IMPORTED_FN, "dep"}}) == 42);
}

TEST_CASE("A test block doesn't resolve while its module's importers are mid-resolution") {
    CHECK(helpers::compile_and_run_tests(
              R"(
            import "lib.gh" as lib;
            test "root" { @expect(lib.k == 1); }
        )",
              {
                  helpers::mock_file{"lib.gh", CYCLE_LIB, "lib"},
                  helpers::mock_file{"first.gh", CYCLE_FIRST, "first"},
                  helpers::mock_file{"meta.gh", CYCLE_META, "meta"},
                  helpers::mock_file{"box.gh", CYCLE_BOX, "box"},
                  helpers::mock_file{"util.gh", CYCLE_UTIL, "util"},
              }) == 0);
}

} // namespace ghoti::tests
