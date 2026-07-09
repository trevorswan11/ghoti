#include <array>
#include <initializer_list>
#include <sstream>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fmt/format.h>
#include <stdx/types.hh>

#include "compiler/syntax/error.hh"
#include "compiler/syntax/keywords.hh"
#include "helpers/ast.hh"
#include "helpers/common.hh"

namespace ghoti::tests {

namespace keywords = syntax::keywords;

TEST_CASE("Non-terminated block") {
    helpers::test_parser_fail(
        "{ ",
        syntax::diagnostic{
            "Expected token RBRACE, found END", syntax::error::UNEXPECTED_TOKEN, 0, 2});
}

namespace {

auto test_decl_fail(std::initializer_list<syntax::keyword_t> modifiers,
                    syntax::diagnostic&&                     expected_error,
                    std::string_view                         init = "a := 2;") -> void {
    std::ostringstream ss;
    for (const auto& keyword : modifiers) { ss << keyword.name << " "; }
    ss << init;
    helpers::test_parser_fail(ss.view(), std::move(expected_error));
}

} // namespace

TEST_CASE("Mutability restrictions") {
    const auto expected_diag = [](usize mod_count = 2) -> syntax::diagnostic {
        return {fmt::format("Exactly one mutability modifier may be used; found {}", mod_count),
                syntax::error::ILLEGAL_DECL_MODIFIERS,
                std::pair{0UZ, 0UZ}};
    };

    constexpr std::array contending_mut{keywords::CONSTEXPR, keywords::VAR, keywords::CONSTANT};
    for (const auto& mut : helpers::combinations(contending_mut)) {
        test_decl_fail({mut.first, mut.second}, expected_diag());
    }
    test_decl_fail({keywords::CONSTEXPR, keywords::VAR, keywords::CONSTANT}, expected_diag(3));
}

TEST_CASE("Constexpr restrictions") {
    const auto expected_diag = [] -> syntax::diagnostic {
        return {"Extern values cannot be known at compile time",
                syntax::error::ILLEGAL_DECL_MODIFIERS,
                std::pair{0UZ, 0UZ}};
    };

    test_decl_fail({keywords::CONSTEXPR, keywords::EXTERN}, expected_diag());
    test_decl_fail({keywords::EXTERN, keywords::CONSTEXPR}, expected_diag());
}

TEST_CASE("ABI/Linkage restrictions") {
    const auto expected_diag = [] -> syntax::diagnostic {
        return {"At most one ABI-related modifier may be used; found 2",
                syntax::error::ILLEGAL_DECL_MODIFIERS,
                std::pair{0UZ, 0UZ}};
    };

    test_decl_fail({keywords::EXPORT, keywords::EXTERN, keywords::VAR}, expected_diag());
    test_decl_fail({keywords::EXTERN, keywords::EXPORT, keywords::VAR}, expected_diag());
}

TEST_CASE("Extern requirements") {
    const auto expected_diag = [] -> syntax::diagnostic {
        return {"Extern declarations may not be value-initialized",
                syntax::error::EXTERN_VALUE_INITIALIZED,
                std::pair{0UZ, 0UZ}};
    };

    test_decl_fail({keywords::EXTERN, keywords::CONSTANT}, expected_diag());
    test_decl_fail({keywords::EXTERN, keywords::VAR}, expected_diag());
}

TEST_CASE("Constant requirements") {
    const auto expected_diag = [] -> syntax::diagnostic {
        return {"Constant non-extern declarations must have an associated value",
                syntax::error::CONST_DECL_MISSING_VALUE,
                std::pair{0UZ, 0UZ}};
    };

    test_decl_fail({keywords::CONSTANT}, expected_diag(), "a: i32;");
    test_decl_fail({keywords::CONSTEXPR}, expected_diag(), "a: i32;");
}

TEST_CASE("Non-terminated decls") {
    helpers::test_parser_fail(
        "var a: i32 = 2",
        syntax::diagnostic{
            "Expected token SEMICOLON, found END", syntax::error::UNEXPECTED_TOKEN, 0, 14});
}

TEST_CASE("Duplicate declaration modifier") {
    helpers::test_parser_fail(
        "var var a: i32;",
        syntax::diagnostic{"Declaration modifiers may only be used once in any order",
                           syntax::error::DUPLICATE_DECL_MODIFIER,
                           std::pair{0UZ, 4UZ}});
}

TEST_CASE("Illegal deferred statements") {
    const auto expected_diag = [] -> syntax::diagnostic {
        return {"Deferred statements must be expressions, discards, or blocks",
                syntax::error::ILLEGAL_DEFERRED_STATEMENT,
                std::pair{0UZ, 6UZ}};
    };

    helpers::test_parser_fail("defer import std;", expected_diag());
    helpers::test_parser_fail("defer return 3;", expected_diag());
    helpers::test_parser_fail("defer var a: i32;", expected_diag());
    helpers::test_parser_fail("defer using a = i32;", expected_diag());
}

TEST_CASE("Missing deferred statements") {
    const auto expected_diag = [] -> syntax::diagnostic {
        return {"Defer statements require an statement to defer",
                syntax::error::DEFER_MISSING_DEFERREE,
                std::pair{0UZ, 0UZ}};
    };

    helpers::test_parser_fail("defer", expected_diag());
    helpers::test_parser_fail("defer;", expected_diag());
}

TEST_CASE("Misplaced correct discardedstatement") {
    helpers::test_parser_fail(
        "_ = import std;",
        syntax::diagnostic{"No prefix parse function for IMPORT(import) found",
                           syntax::error::MISSING_PREFIX_PARSER,
                           std::pair{0UZ, 4UZ}});
}

TEST_CASE("Misplaced incorrect discarded statement") {
    helpers::test_parser_fail(
        "_ = import 3;",
        syntax::diagnostic{"No prefix parse function for IMPORT(import) found",
                           syntax::error::MISSING_PREFIX_PARSER,
                           std::pair{0UZ, 4UZ}});
}

TEST_CASE("Missing discardee") {
    const auto expected_diag = [] -> syntax::diagnostic {
        return {"Discarded statements must have a statement to discard",
                syntax::error::DISCARD_MISSING_DISCARDEE,
                std::pair{0UZ, 2UZ}};
    };

    helpers::test_parser_fail("_ = ", expected_diag());
    helpers::test_parser_fail("_ = ;", expected_diag());
}

TEST_CASE("Incorrect library imports") {
    const auto expected_diag = [] -> syntax::diagnostic {
        return {"Imported payloads may only be filename strings or module identifiers",
                syntax::error::ILLEGAL_IMPORT_TYPE,
                std::pair{0UZ, 7UZ}};
    };

    helpers::test_parser_fail("import 2;", expected_diag());
    helpers::test_parser_fail("import as 2;", expected_diag());
    helpers::test_parser_fail("import 2 as 3;", expected_diag());

    helpers::test_parser_fail(
        "import std as 2;",
        syntax::diagnostic{
            "Expected token IDENT, found INT_10", syntax::error::UNEXPECTED_TOKEN, 0, 14});
}

TEST_CASE("Incorrect file imports ") {
    helpers::test_parser_fail(R"(import "";)",
                              syntax::diagnostic{"File import names cannot be empty",
                                                 syntax::error::EMPTY_FILE_IMPORT,
                                                 std::pair{0UZ, 7UZ}});

    helpers::test_parser_fail(R"(import "" as e;)",
                              syntax::diagnostic{"File import names cannot be empty",
                                                 syntax::error::EMPTY_FILE_IMPORT,
                                                 std::pair{0UZ, 7UZ}});

    helpers::test_parser_fail(
        R"(import "ast/node.p";)",
        syntax::diagnostic{"All file imports must be aliased to an identifier",
                           syntax::error::FILE_IMPORT_MISSING_ALIAS,
                           std::pair{0UZ, 0UZ}});

    helpers::test_parser_fail(
        R"(import "ast/node.p" as 2;)",
        syntax::diagnostic{
            "Expected token IDENT, found INT_10", syntax::error::UNEXPECTED_TOKEN, 0, 23});
}

TEST_CASE("Non-terminated imports") {
    helpers::test_parser_fail(
        "import std",
        syntax::diagnostic{
            "Expected token SEMICOLON, found END", syntax::error::UNEXPECTED_TOKEN, 0, 10});
}

TEST_CASE("Incorrectly terminated jumps") {
    using namespace std::string_view_literals;
    const auto input{GENERATE("return"sv, "continue"sv, "break"sv)};
    helpers::test_parser_fail(input,
                              syntax::diagnostic{"Expected token SEMICOLON, found END",
                                                 syntax::error::UNEXPECTED_TOKEN,
                                                 std::pair{0UZ, input.size()}});
}

TEST_CASE("Illegal control flow") {
    helpers::test_parser_fail("continue 4;",
                              syntax::diagnostic{"Continue statements may only contain labels",
                                                 syntax::error::VALUED_CONTINUE,
                                                 std::pair{0UZ, 0UZ}});

    helpers::test_parser_fail("break 4;",
                              syntax::diagnostic{"Valued break statements must be labeled",
                                                 syntax::error::VALUED_BREAK_MISSING_LABEL,
                                                 std::pair{0UZ, 0UZ}});

    helpers::test_parser_fail(
        "return return",
        syntax::diagnostic{"No prefix parse function for RETURN(return) found",
                           syntax::error::MISSING_PREFIX_PARSER,
                           std::pair{0UZ, 7UZ}});
}

TEST_CASE("Non-terminated test") {
    helpers::test_parser_fail(
        "test {",
        syntax::diagnostic{
            "Expected token RBRACE, found END", syntax::error::UNEXPECTED_TOKEN, 0, 6});
}

TEST_CASE("Empty test description") {
    helpers::test_parser_fail(R"(test "" {};)",
                              syntax::diagnostic{"Test descriptions may not be empty when present",
                                                 syntax::error::EMPTY_TEST_DESCRIPTION,
                                                 std::pair{0UZ, 5UZ}});
}

TEST_CASE("Missing alias") {
    helpers::test_parser_fail(
        "using &[0x2UZ][N]*E;",
        syntax::diagnostic{
            "Expected token IDENT, found BW_AND", syntax::error::UNEXPECTED_TOKEN, 0, 6});
}

TEST_CASE("Missing type") {
    helpers::test_parser_fail(
        "using T;",
        syntax::diagnostic{
            "Expected token ASSIGN, found SEMICOLON", syntax::error::UNEXPECTED_TOKEN, 0, 7});
}

TEST_CASE("Illegal identifier alias") {
    helpers::test_parser_fail(
        "using type = T;",
        syntax::diagnostic{
            "Expected token IDENT, found TYPE_TYPE", syntax::error::UNEXPECTED_TOKEN, 0, 6});
}

} // namespace ghoti::tests
