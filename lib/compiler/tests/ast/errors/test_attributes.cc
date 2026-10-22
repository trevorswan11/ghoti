#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/arena.hh"
#include "compiler/ast/ast.hh"
#include "compiler/syntax/error.hh"
#include "compiler/syntax/parser.hh"
#include "helpers/ast.hh"

namespace ghoti::tests {

TEST_CASE("Attribute list names must be known attributes") {
    helpers::test_parser_fail("@[bogus] const a = 1;",
                              syntax::diagnostic{"Unknown attribute 'bogus'",
                                                 syntax::error::UNKNOWN_ATTRIBUTE,
                                                 std::pair{0UZ, 2UZ}});
    helpers::test_parser_fail("@[1] const a = 1;",
                              syntax::diagnostic{"Expected an attribute name, found an integer "
                                                 "literal",
                                                 syntax::error::UNKNOWN_ATTRIBUTE,
                                                 std::pair{0UZ, 2UZ}});
}

TEST_CASE("Attribute arguments are checked against the attribute's arity") {
    helpers::test_parser_fail(
        "@[discardable(true, false)] const f = fn(): i32 { return 0; };",
        syntax::diagnostic{"Attribute 'discardable' takes 0 to 1 argument; found 2",
                           syntax::error::ATTRIBUTE_ARITY,
                           std::pair{0UZ, 2UZ}});
}

TEST_CASE("An attribute may appear once, in a single list") {
    helpers::test_parser_fail("@[discardable, discardable] const f = fn(): i32 { return 0; };",
                              syntax::diagnostic{"Attribute 'discardable' may only appear once",
                                                 syntax::error::DUPLICATE_ATTRIBUTE,
                                                 std::pair{0UZ, 15UZ}});
    helpers::test_parser_fail(
        "@[discardable] @[discardable] const f = fn(): i32 { return 0; };",
        syntax::diagnostic{"Only one attribute list may be applied; merge them into one",
                           syntax::error::DUPLICATE_ATTRIBUTE,
                           std::pair{0UZ, 15UZ}});
}

TEST_CASE("An attribute list may not be empty") {
    helpers::test_parser_fail("@[] const a = 1;",
                              syntax::diagnostic{"An attribute list may not be empty",
                                                 syntax::error::MISPLACED_ATTRIBUTES,
                                                 std::pair{0UZ, 0UZ}});
}

TEST_CASE("An attribute list must precede a declaration or function literal") {
    helpers::test_parser_fail("@[discardable] 1 + 2;",
                              syntax::diagnostic{"An attribute list must precede a declaration",
                                                 syntax::error::MISPLACED_ATTRIBUTES,
                                                 std::pair{0UZ, 15UZ}});
    helpers::test_parser_fail(
        "const f = @[discardable] 5;",
        syntax::diagnostic{
            "An attribute list in expression position must precede a function literal",
            syntax::error::MISPLACED_ATTRIBUTES,
            std::pair{0UZ, 25UZ}});
    helpers::test_parser_fail(
        "const f = @[discardable] fn(): i32;",
        syntax::diagnostic{"Attributes apply to function definitions, not function types",
                           syntax::error::MISPLACED_ATTRIBUTES,
                           std::pair{0UZ, 25UZ}});
}

TEST_CASE("The removed `@discardable` modifier no longer parses") {
    syntax::parser p{"@discardable const f = fn(): i32 { return 0; };"};
    ghoti::arena   arena;
    ast::AST       ast;
    CHECK_FALSE(p.consume(ast, arena).empty());
}

} // namespace ghoti::tests
