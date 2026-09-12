#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("E2E raw identifier: a keyword-named binding is usable") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const @"type" := 40;
            const @"match" := 2;
            return @"type" + @"match";
        };
    )") == 42);
}

TEST_CASE("E2E raw identifier: a raw name and its bare spelling denote the same symbol") {
    CHECK(helpers::compile_and_run(R"(
        const plain := 42;

        pub const main := fn(): i32 {
            return @"plain";
        };
    )") == 42);
}

TEST_CASE("E2E raw identifier: keyword-named struct fields round-trip through codegen") {
    CHECK(helpers::compile_and_run(R"(
        const Box := struct {
            @"struct": i32,
            @"fn": i32,
        };

        pub const main := fn(): i32 {
            const b: Box = .{ .@"struct" = 30, .@"fn" = 12 };
            return b.@"struct" + b.@"fn";
        };
    )") == 42);
}

TEST_CASE("E2E raw identifier: a primitive spelling can name a user binding") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const @"i32": i32 = 42;
            return @"i32";
        };
    )") == 42);
}

} // namespace ghoti::tests
