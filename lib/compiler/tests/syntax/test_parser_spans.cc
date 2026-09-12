#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/types.hh>

#include "compiler/arena.hh"
#include "compiler/ast/ast.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/syntax/parser.hh"
#include "helpers/ast.hh"

namespace ghoti::tests {

TEST_CASE("every node has an end_location_of that is not before its start location_of") {
    ghoti::arena arena;
    auto         ast{helpers::parse("const x := foo(1, 2) + bar.baz;\n"
                                    "const y := struct { a: i32 };\n",
                            arena)};

    for (const auto root : ast) {
        const auto& start{ast.location_of(root)};
        const auto& end{ast.end_location_of(root)};
        CHECK((end.line > start.line || (end.line == start.line && end.column >= start.column)));
    }
}

TEST_CASE("a call_expr's span ends just past its closing paren") {
    ghoti::arena arena;
    auto         ast{helpers::parse("const x := foo(1, 2);\n", arena)};
    const auto&  decl{ast.get_as<ast::decl_stmt>(*ast.begin())};

    REQUIRE(decl.value);
    const auto& call{ast.get_as<ast::call_expr>(*decl.value)};
    const auto& end{ast.end_location_of(*decl.value)};

    CHECK(end.line == 0);
    CHECK(end.column == 20);
    CHECK(call.arguments.size() == 2);
}

TEST_CASE("a binary_expr's span starts at its lhs, not its operator") {
    ghoti::arena arena;
    auto         ast{helpers::parse("const x := 1 + 2;\n", arena)};
    const auto&  decl{ast.get_as<ast::decl_stmt>(*ast.begin())};

    REQUIRE(decl.value);
    const auto& bin{ast.get_as<ast::binary_expr>(*decl.value)};
    const auto& start{ast.location_of(*decl.value)};
    CHECK(bin.rhs.is<ast::int_literal_expr>());

    CHECK(start.line == 0);
    CHECK(start.column == 11);
}

TEST_CASE("an assignment_expr's span starts at its lhs, not its operator") {
    ghoti::arena arena;
    auto         ast{helpers::parse("const f := fn(): void {\n"
                                    "    var x := 1;\n"
                                    "    x = 2;\n"
                                    "};\n",
                            arena)};
    const auto&  decl{ast.get_as<ast::decl_stmt>(*ast.begin())};
    REQUIRE(decl.value);
    const auto& fn{ast.get_as<ast::function_expr>(*decl.value)};
    const auto& body{ast.get_as<ast::block_stmt>(*fn.body)};

    REQUIRE(body.statements.size() == 2);
    const auto& assign_stmt{ast.get_as<ast::expr_stmt>(*body.statements[1])};
    const auto& assign{ast.get_as<ast::assignment_expr>(*assign_stmt.expression)};
    const auto& start{ast.location_of(*assign_stmt.expression)};
    CHECK(assign.lhs.is<ast::identifier_expr>());

    CHECK(start.line == 2);
    CHECK(start.column == 4);
}

TEST_CASE("a postfix unwrap_expr's span starts at its operand and ends past its operator") {
    ghoti::arena arena;
    auto         ast{helpers::parse("const x := abc?;\n", arena)};
    const auto&  decl{ast.get_as<ast::decl_stmt>(*ast.begin())};

    REQUIRE(decl.value);
    const auto& unwrap{ast.get_as<ast::unwrap_expr>(*decl.value)};
    CHECK((**decl.value).get_token_type() == syntax::token_type_t::QUESTION);
    CHECK(unwrap.operand.is<ast::identifier_expr>());

    const auto& start{ast.location_of(*decl.value)};
    const auto& end{ast.end_location_of(*decl.value)};
    CHECK(start.line == 0);
    CHECK(start.column == 11); // the `a` of `abc`, not the `?`
    CHECK(end.line == 0);
    CHECK(end.column == 15); // just past the `?`
}

TEST_CASE("a block_stmt's span ends just past its closing brace") {
    ghoti::arena arena;
    auto         ast{helpers::parse("const f := fn(): void {\n"
                                    "    return;\n"
                                    "};\n",
                            arena)};
    const auto&  decl{ast.get_as<ast::decl_stmt>(*ast.begin())};

    REQUIRE(decl.value);
    const auto& fn{ast.get_as<ast::function_expr>(*decl.value)};
    const auto& end{ast.end_location_of(fn.body)};

    CHECK(end.line == 2);
    CHECK(end.column == 1);
}

TEST_CASE("interface_expr records associated items and required vs default methods") {
    ghoti::arena arena;
    auto         ast{helpers::parse(R"(const W := interface {
    Error: type;
    Item: type = u8;
    const cap: usize = 4096;
    pub const write := fn(&mut self, b: []u8): R;
    const dbg := fn(&self): []u8;
    pub const writeAll := fn(&mut self, b: []u8): R { return self.write(b); };
};
)",
                            arena)};

    const auto& decl{ast.get_as<ast::decl_stmt>(*ast.begin())};
    REQUIRE(decl.value);
    const auto& iface{ast.get_as<ast::interface_expr>(*decl.value)};

    REQUIRE(iface.assoc_types.size() == 2);
    CHECK_FALSE(iface.assoc_types[0].default_type.has_value());
    CHECK(iface.assoc_types[1].default_type.has_value());

    REQUIRE(iface.assoc_consts.size() == 1);
    CHECK(iface.assoc_consts[0].default_value.has_value());

    REQUIRE(iface.methods.size() == 3);
    CHECK(iface.methods[0].is_public());
    CHECK_FALSE(iface.methods[1].is_public());
    CHECK(ast.get_as<ast::function_expr>(*iface.methods[0].signature).is_type_expr);
    CHECK_FALSE(ast.get_as<ast::function_expr>(*iface.methods[2].signature).is_type_expr);
}

TEST_CASE("impl_stmt distinguishes trait, inherent, and parameterized forms") {
    ghoti::arena arena;
    auto        inherent{helpers::parse("impl File { pub const f := fn(&self): void {}; }", arena)};
    const auto& i0{inherent.get_as<ast::impl_stmt>(*inherent.begin())};
    CHECK_FALSE(i0.interface_type.has_value());
    CHECK(i0.impl_params.empty());
    CHECK(i0.members.size() == 1);

    auto        trait{helpers::parse(
        "impl Writer for File { pub const write := fn(&mut self, b: []u8): R { c; }; }", arena)};
    const auto& i1{trait.get_as<ast::impl_stmt>(*trait.begin())};
    CHECK(i1.interface_type.has_value());

    auto        param{helpers::parse("impl(H: type) Writer(H) { const cap := 8; }\n", arena)};
    const auto& i2{param.get_as<ast::impl_stmt>(*param.begin())};
    REQUIRE(i2.impl_params.size() == 1);
    CHECK_FALSE(i2.interface_type.has_value());
}

TEST_CASE("impl and interface parse errors recover") {
    const auto parse_errs{[](std::string_view source) -> usize {
        syntax::parser p{source};
        ghoti::arena   arena;
        ast::AST       parsed;
        return p.consume(parsed, arena).size();
    }};

    CHECK(parse_errs("impl Foo;\n") > 0);                   // missing `{ ... }` body
    CHECK(parse_errs("impl for Bar {}\n") > 0);             // missing interface before `for`
    CHECK(parse_errs("const I := interface { x };\n") > 0); // malformed member
}

} // namespace ghoti::tests
