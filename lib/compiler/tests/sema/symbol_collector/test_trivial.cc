#include <array>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/sema/error.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/token_type.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

using helpers::mock_file;

TEST_CASE("Array/Index collection") {
    helpers::collect_and_check("const a := [2UZ]i32{A, B, }; const b := a[0];");
    helpers::collect_and_check("const a := [_]^N{a, b, c, d, e, }; const b := a[2];");
    helpers::collect_and_check("const a := [_]^N{a, b, c, d, if (e > f) g else h, };");
    helpers::collect_and_check("const a := [2UZ]i32{A, match (B) { c => d }, };");
}

TEST_CASE("Builtin calling collection") {
    for (const auto& builtin : syntax::builtins::ALL_TOKEN_TYPES) {
        if (builtin == syntax::token_type_t::BUILTIN_EXPECT ||
            builtin == syntax::token_type_t::BUILTIN_REQUIRE ||
            builtin == syntax::token_type_t::BUILTIN_SKIP) {
            continue;
        }
        const auto input{
            fmt::format("const a := {}(match (b) {{ c => d, e => f }}, g, if (5 <= h) i else j);",
                        *syntax::get_builtin_opt(builtin))};
        helpers::collect_and_check(input);
    }
}

TEST_CASE("Initializer collection") {
    helpers::collect_and_check("const a := T{ .b = 2 };");
    helpers::collect_and_check("const a: T = .{ .b = 2 };");
    helpers::collect_and_check("const a := T{ .b = 3u + v, .c = if (5 <= h) i else j };");
}

TEST_CASE("Discard statement collection") {
    helpers::collect_and_check("_ = 0..20;");
    helpers::collect_and_check("const a := fn(&self, c: type): void { _ = c; };");
}

TEST_CASE("Prefix expression collection") {
    helpers::collect_and_check("const a := &b;");
    helpers::collect_and_check("const a := *b;");
    helpers::collect_and_check("const a := !b;");
}

TEST_CASE("Duplicate identifiers") {
    helpers::test_collector_fail(
        "const a := 2; import a;",
        helpers::make_vector<mock_file>(
            mock_file{.path = "a.gh", .source = "const foo := bar;", .name = "a"}),
        sema::diagnostic{"Redeclaration of symbol 'a'; previous declaration here: 1:7",
                         sema::error::IDENTIFIER_REDECLARATION,
                         std::pair{0UZ, 14UZ}});
}

TEST_CASE("Semantically illegal statements") {
    helpers::test_collector_fail("{}",
                                 sema::diagnostic{"Cannot have block at the top level",
                                                  sema::error::ILLEGAL_TOP_LEVEL_STATEMENT,
                                                  std::pair{0UZ, 0UZ}});

    helpers::test_collector_fail("defer 2;",
                                 sema::diagnostic{"Cannot have defer outside of a function's scope",
                                                  sema::error::ILLEGAL_TOP_LEVEL_STATEMENT,
                                                  std::pair{0UZ, 0UZ}});

    helpers::test_collector_fail("return 2;",
                                 sema::diagnostic{"Cannot return outside of a function",
                                                  sema::error::ILLEGAL_CONTROL_FLOW,
                                                  std::pair{0UZ, 0UZ}});

    helpers::test_collector_fail("break; continue;",
                                 sema::diagnostic{"Cannot break outside of a loop or label",
                                                  sema::error::ILLEGAL_CONTROL_FLOW,
                                                  std::pair{0UZ, 0UZ}},
                                 sema::diagnostic{"Cannot continue outside of a loop",
                                                  sema::error::ILLEGAL_CONTROL_FLOW,
                                                  std::pair{0UZ, 7UZ}});
}

using namespace std::string_view_literals;

constexpr std::array RESTRICTED_INPUTS{
    std::pair{"a := struct { var b := 2; };"sv, "struct"sv},
    std::pair{"a := enum { A };"sv, "enum"sv},
    std::pair{"a := union { b: bool };"sv, "union"sv},
};

TEST_CASE("Redundant constexpr usage for declarations") {
    for (const auto& [input, desc] : RESTRICTED_INPUTS) {
        helpers::test_collector_fail(
            fmt::format("constexpr {}", input),
            sema::diagnostic{fmt::format("All {}s are implicitly constexpr", desc),
                             sema::error::REDUNDANT_CONSTEXPR,
                             std::pair{0UZ, 0UZ}});
    }
}

TEST_CASE("Restricted non-const top level declarations") {
    for (const auto& [input, desc] : RESTRICTED_INPUTS) {
        helpers::test_collector_fail(
            fmt::format("var {}", input),
            sema::diagnostic{fmt::format("All {}s must be marked const", desc),
                             sema::error::ILLEGAL_NON_CONST_STATEMENT,
                             std::pair{0UZ, 0UZ}});
    }
}

TEST_CASE("Restricted non-const member types") {
    for (const auto& [input, desc] : RESTRICTED_INPUTS) {
        helpers::test_collector_fail(
            fmt::format("const S := struct {{ var {} }};", input),
            sema::diagnostic{fmt::format("All {}s must be marked const", desc),
                             sema::error::ILLEGAL_NON_CONST_STATEMENT,
                             std::pair{0UZ, 20UZ}});
    }
}

} // namespace ghoti::tests
