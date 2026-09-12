#include <sstream>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "compiler/ast/dumper.hh"
#include "compiler/syntax/error.hh"
#include "compiler/syntax/parser.hh"
#include "helpers/common.hh"

namespace ghoti::tests {

constexpr std::string_view golden_input{
#include "ast/golden.gh.inc"
};

constexpr std::string_view expected{
#include "ast/dump.inc"
};

TEST_CASE("Comprehensive dump") {
    syntax::parser p{golden_input};
    ghoti::arena   arena;
    ast::AST       ast;
    auto           errors{p.consume(ast, arena)};
    helpers::check_errors<syntax::diagnostic>(errors);

    std::ostringstream oss;
    ast::dumper        dumper{ast, oss};
    dumper.dump();
    CHECK(expected == oss.view());
}

} // namespace ghoti::tests
