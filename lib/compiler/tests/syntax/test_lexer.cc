#include <algorithm>
#include <initializer_list>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/lexer.hh"
#include "compiler/syntax/token.hh"
#include "compiler/syntax/token_type.hh"

namespace ghoti::tests {

using syntax::token_type_t;
using expected_lexeme = std::pair<token_type_t, std::string_view>;

namespace {

auto test_lexer(std::string_view input, std::initializer_list<expected_lexeme> expecteds) -> void {
    syntax::lexer l{input};
    for (const auto& [expected_tok, expected_slice] : expecteds) {
        const auto token{l.advance()};
        CHECK(expected_tok == token.type);
        CHECK(expected_slice == token.slice);
    }

    // Should be true regardless of caller putting END in their list
    const auto& end_tok{l.advance()};
    CHECK(end_tok.type == token_type_t::END);
    CHECK(end_tok.slice == "");
}

} // namespace

TEST_CASE("Lexing illegal characters") {
    syntax::lexer l{"月😭🎶"};
    for (auto it{l.begin()}; it != l.end(); ++it) { CHECK(it->type == token_type_t::ILLEGAL); }
}

TEST_CASE("Lexing symbols") {
    test_lexer("=+(){}[],;: !?-/*<>_",
               {
                   {token_type_t::ASSIGN, "="},     {token_type_t::PLUS, "+"},
                   {token_type_t::LPAREN, "("},     {token_type_t::RPAREN, ")"},
                   {token_type_t::LBRACE, "{"},     {token_type_t::RBRACE, "}"},
                   {token_type_t::LBRACKET, "["},   {token_type_t::RBRACKET, "]"},
                   {token_type_t::COMMA, ","},      {token_type_t::SEMICOLON, ";"},
                   {token_type_t::COLON, ":"},      {token_type_t::BANG, "!"},
                   {token_type_t::QUESTION, "?"},   {token_type_t::MINUS, "-"},
                   {token_type_t::SLASH, "/"},      {token_type_t::STAR, "*"},
                   {token_type_t::LT, "<"},         {token_type_t::GT, ">"},
                   {token_type_t::UNDERSCORE, "_"},
               });
}

TEST_CASE("Postfix unwrap operators do not disturb neighboring tokens") {
    test_lexer("a?.b",
               {
                   {token_type_t::IDENT, "a"},
                   {token_type_t::QUESTION, "?"},
                   {token_type_t::DOT, "."},
                   {token_type_t::IDENT, "b"},
               });

    // `!` stays greedy: `!=` is still a single not-equal token
    test_lexer("a! != b!",
               {
                   {token_type_t::IDENT, "a"},
                   {token_type_t::BANG, "!"},
                   {token_type_t::NEQ, "!="},
                   {token_type_t::IDENT, "b"},
                   {token_type_t::BANG, "!"},
               });
}

TEST_CASE("Lexing identifiers with leading underscores") {
    test_lexer("_foo __bar _123 _",
               {
                   {token_type_t::IDENT, "_foo"},
                   {token_type_t::IDENT, "__bar"},
                   {token_type_t::IDENT, "_123"},
                   {token_type_t::UNDERSCORE, "_"},
               });
}

TEST_CASE("Lexing basic language snippet") {
    test_lexer("const five := 5;\n"
               "var ten := 10;\n\n"
               "var add := fn(x: i32, y: i32): i32 {\n"
               "   return x + y;\n"
               "};\n\n"
               "var result := add(five, ten);\n"
               "var fs: f64 = 4.2;",
               {
                   {token_type_t::CONSTANT, "const"}, {token_type_t::IDENT, "five"},
                   {token_type_t::WALRUS, ":="},      {token_type_t::INT_10, "5"},
                   {token_type_t::SEMICOLON, ";"},    {token_type_t::VAR, "var"},
                   {token_type_t::IDENT, "ten"},      {token_type_t::WALRUS, ":="},
                   {token_type_t::INT_10, "10"},      {token_type_t::SEMICOLON, ";"},
                   {token_type_t::VAR, "var"},        {token_type_t::IDENT, "add"},
                   {token_type_t::WALRUS, ":="},      {token_type_t::FUNCTION, "fn"},
                   {token_type_t::LPAREN, "("},       {token_type_t::IDENT, "x"},
                   {token_type_t::COLON, ":"},        {token_type_t::I32_TYPE, "i32"},
                   {token_type_t::COMMA, ","},        {token_type_t::IDENT, "y"},
                   {token_type_t::COLON, ":"},        {token_type_t::I32_TYPE, "i32"},
                   {token_type_t::RPAREN, ")"},       {token_type_t::COLON, ":"},
                   {token_type_t::I32_TYPE, "i32"},   {token_type_t::LBRACE, "{"},
                   {token_type_t::RETURN, "return"},  {token_type_t::IDENT, "x"},
                   {token_type_t::PLUS, "+"},         {token_type_t::IDENT, "y"},
                   {token_type_t::SEMICOLON, ";"},    {token_type_t::RBRACE, "}"},
                   {token_type_t::SEMICOLON, ";"},    {token_type_t::VAR, "var"},
                   {token_type_t::IDENT, "result"},   {token_type_t::WALRUS, ":="},
                   {token_type_t::IDENT, "add"},      {token_type_t::LPAREN, "("},
                   {token_type_t::IDENT, "five"},     {token_type_t::COMMA, ","},
                   {token_type_t::IDENT, "ten"},      {token_type_t::RPAREN, ")"},
                   {token_type_t::SEMICOLON, ";"},    {token_type_t::VAR, "var"},
                   {token_type_t::IDENT, "fs"},       {token_type_t::COLON, ":"},
                   {token_type_t::F64_TYPE, "f64"},   {token_type_t::ASSIGN, "="},
                   {token_type_t::F64, "4.2"},        {token_type_t::SEMICOLON, ";"},
               });
}

TEST_CASE("Lexing numbers") {
    test_lexer("0 123 3.14 42.0 1e20 1.e-3 2.3901E4f 1e.",
               {
                   {token_type_t::INT_10, "0"},
                   {token_type_t::INT_10, "123"},
                   {token_type_t::F64, "3.14"},
                   {token_type_t::F64, "42.0"},
                   {token_type_t::F64, "1e20"},
                   {token_type_t::F64, "1.e-3"},
                   {token_type_t::F32, "2.3901E4f"},
                   {token_type_t::INT_10, "1"},
                   {token_type_t::IDENT, "e"},
                   {token_type_t::DOT, "."},
               });
}

TEST_CASE("Lexing illegal floats") {
    test_lexer(".0 1..2 3.4.5 3.4u 5f",
               {
                   {token_type_t::DOT, "."},
                   {token_type_t::INT_10, "0"},
                   {token_type_t::INT_10, "1"},
                   {token_type_t::DOT_DOT, ".."},
                   {token_type_t::INT_10, "2"},
                   {token_type_t::F64, "3.4"},
                   {token_type_t::DOT, "."},
                   {token_type_t::INT_10, "5"},
                   {token_type_t::F64, "3.4"},
                   {token_type_t::IDENT, "u"},
                   {token_type_t::F32, "5f"},
               });
}

TEST_CASE("Lexing signed int variants") {
    test_lexer("0b1010 0o17 0O17 42 0x2A 0X2A 0b 0x 0o",
               {
                   {token_type_t::INT_2, "0b1010"},
                   {token_type_t::INT_8, "0o17"},
                   {token_type_t::INT_8, "0O17"},
                   {token_type_t::INT_10, "42"},
                   {token_type_t::INT_16, "0x2A"},
                   {token_type_t::INT_16, "0X2A"},
                   {token_type_t::ILLEGAL, "0b"},
                   {token_type_t::ILLEGAL, "0x"},
                   {token_type_t::ILLEGAL, "0o"},
               });
}

TEST_CASE("Lexing unsigned int variants") {
    test_lexer("0b1010u 0b1010uz 0o17u 0o17uz 0O17u 42u 42UZ 0x2AU 0X2Au 123ufoo 0bu 0xu 0ou",
               {
                   {token_type_t::UINT_2, "0b1010u"},
                   {token_type_t::UZINT_2, "0b1010uz"},
                   {token_type_t::UINT_8, "0o17u"},
                   {token_type_t::UZINT_8, "0o17uz"},
                   {token_type_t::UINT_8, "0O17u"},
                   {token_type_t::UINT_10, "42u"},
                   {token_type_t::UZINT_10, "42UZ"},
                   {token_type_t::UINT_16, "0x2AU"},
                   {token_type_t::UINT_16, "0X2Au"},
                   {token_type_t::UINT_10, "123u"},
                   {token_type_t::IDENT, "foo"},
                   {token_type_t::ILLEGAL, "0b"},
                   {token_type_t::IDENT, "u"},
                   {token_type_t::ILLEGAL, "0x"},
                   {token_type_t::IDENT, "u"},
                   {token_type_t::ILLEGAL, "0o"},
                   {token_type_t::IDENT, "u"},
               });
}

TEST_CASE("Lexing int bit-width variants") {
    test_lexer("2 2l 2z 2u 2ul 2uz",
               {
                   {token_type_t::INT_10, "2"},
                   {token_type_t::LINT_10, "2l"},
                   {token_type_t::ZINT_10, "2z"},
                   {token_type_t::UINT_10, "2u"},
                   {token_type_t::ULINT_10, "2ul"},
                   {token_type_t::UZINT_10, "2uz"},
               });
}

TEST_CASE("Lexing underscore-separated numbers") {
    test_lexer("0_1 123_2 3.14_159 42.0_11f 0b11_00_11 0xEEEE_FFFFuz 0o7_233u",
               {
                   {token_type_t::INT_10, "0_1"},
                   {token_type_t::INT_10, "123_2"},
                   {token_type_t::F64, "3.14_159"},
                   {token_type_t::F32, "42.0_11f"},
                   {token_type_t::INT_2, "0b11_00_11"},
                   {token_type_t::UZINT_16, "0xEEEE_FFFFuz"},
                   {token_type_t::UINT_8, "0o7_233u"},
               });
}

TEST_CASE("Lexing illegal underscored numbers") {
    test_lexer("1234_.1 121.3_ _12",
               {
                   {token_type_t::ILLEGAL, "1234_"},
                   {token_type_t::DOT, "."},
                   {token_type_t::INT_10, "1"},
                   {token_type_t::ILLEGAL, "121.3_"},
                   {token_type_t::IDENT, "_12"},
               });
}

TEST_CASE("Lexing keywords") {
    test_lexer("and or pub extern export volatile mut "
               "i32 i64 isize u32 u64 usize f32 f64 u8 bool void type test asm",
               {
                   {token_type_t::BOOLEAN_AND, "and"},  {token_type_t::BOOLEAN_OR, "or"},
                   {token_type_t::PUBLIC, "pub"},       {token_type_t::EXTERN, "extern"},
                   {token_type_t::EXPORT, "export"},    {token_type_t::VOLATILE, "volatile"},
                   {token_type_t::MUT, "mut"},          {token_type_t::I32_TYPE, "i32"},
                   {token_type_t::I64_TYPE, "i64"},     {token_type_t::ISIZE_TYPE, "isize"},
                   {token_type_t::U32_TYPE, "u32"},     {token_type_t::U64_TYPE, "u64"},
                   {token_type_t::USIZE_TYPE, "usize"}, {token_type_t::F32_TYPE, "f32"},
                   {token_type_t::F64_TYPE, "f64"},     {token_type_t::U8_TYPE, "u8"},
                   {token_type_t::BOOL_TYPE, "bool"},   {token_type_t::VOID_TYPE, "void"},
                   {token_type_t::TYPE_TYPE, "type"},   {token_type_t::TEST, "test"},
                   {token_type_t::ASM, "asm"},
               });
}

TEST_CASE("Lexing comments") {
    test_lexer("const five := 5;\n"
               "var ten_10 := 10;\n\n"
               "// BOL\n"
               "var result := add(five, ten); // EOL\n"
               "var four_and_some := 4.2f;",
               {
                   {token_type_t::CONSTANT, "const"}, {token_type_t::IDENT, "five"},
                   {token_type_t::WALRUS, ":="},      {token_type_t::INT_10, "5"},
                   {token_type_t::SEMICOLON, ";"},    {token_type_t::VAR, "var"},
                   {token_type_t::IDENT, "ten_10"},   {token_type_t::WALRUS, ":="},
                   {token_type_t::INT_10, "10"},      {token_type_t::SEMICOLON, ";"},
                   {token_type_t::COMMENT, " BOL"},   {token_type_t::VAR, "var"},
                   {token_type_t::IDENT, "result"},   {token_type_t::WALRUS, ":="},
                   {token_type_t::IDENT, "add"},      {token_type_t::LPAREN, "("},
                   {token_type_t::IDENT, "five"},     {token_type_t::COMMA, ","},
                   {token_type_t::IDENT, "ten"},      {token_type_t::RPAREN, ")"},
                   {token_type_t::SEMICOLON, ";"},    {token_type_t::COMMENT, " EOL"},
                   {token_type_t::VAR, "var"},        {token_type_t::IDENT, "four_and_some"},
                   {token_type_t::WALRUS, ":="},      {token_type_t::F32, "4.2f"},
                   {token_type_t::SEMICOLON, ";"},
               });
}

TEST_CASE("Lexing character literals") {
    test_lexer("if'e' else'\\'\nreturn'\\r' break'\\n'\n"
               "continue'\\0' for'\\'' while'\\\\' const''\n"
               "var'asd'",
               {
                   {token_type_t::IF, "if"},
                   {token_type_t::U8, "'e'"},
                   {token_type_t::ELSE, "else"},
                   {token_type_t::ILLEGAL, "'\\'"},
                   {token_type_t::RETURN, "return"},
                   {token_type_t::U8, "'\\r'"},
                   {token_type_t::BREAK, "break"},
                   {token_type_t::U8, "'\\n'"},
                   {token_type_t::CONTINUE, "continue"},
                   {token_type_t::U8, "'\\0'"},
                   {token_type_t::FOR, "for"},
                   {token_type_t::U8, "'\\''"},
                   {token_type_t::WHILE, "while"},
                   {token_type_t::U8, "'\\\\'"},
                   {token_type_t::CONSTANT, "const"},
                   {token_type_t::ILLEGAL, "'"},
                   {token_type_t::ILLEGAL, "'"},
                   {token_type_t::VAR, "var"},
                   {token_type_t::ILLEGAL, "'asd'"},
               });
}

TEST_CASE("Lexing string literals") {
    test_lexer(
        R"("This is a string";const five := "Hello, World!";var ten: [:0]u8 = "Hello\n, World!\0";var one := "Hello, World!;)",
        {
            {token_type_t::STRING, R"("This is a string")"},
            {token_type_t::SEMICOLON, ";"},
            {token_type_t::CONSTANT, "const"},
            {token_type_t::IDENT, "five"},
            {token_type_t::WALRUS, ":="},
            {token_type_t::STRING, R"("Hello, World!")"},
            {token_type_t::SEMICOLON, ";"},
            {token_type_t::VAR, "var"},
            {token_type_t::IDENT, "ten"},
            {token_type_t::COLON, ":"},
            {token_type_t::LBRACKET, "["},
            {token_type_t::NULL_TERMINATED, ":0"},
            {token_type_t::RBRACKET, "]"},
            {token_type_t::U8_TYPE, "u8"},
            {token_type_t::ASSIGN, "="},
            {token_type_t::STRING, R"("Hello\n, World!\0")"},
            {token_type_t::SEMICOLON, ";"},
            {token_type_t::VAR, "var"},
            {token_type_t::IDENT, "one"},
            {token_type_t::WALRUS, ":="},
            {token_type_t::ILLEGAL, R"("Hello, World!;)"},
        });
}

TEST_CASE("Lexing multiline string literals") {
    test_lexer("const five := \\\\Multiline stringing\n"
               ";\n"
               "var ten := \\\\Multiline stringing\n"
               "\\\\Continuation\n"
               ";\n"
               "const one := \\\\Nesting \" \' \\ [] const var\n"
               "\\\\\n"
               ";\n",
               {
                   {token_type_t::CONSTANT, "const"},
                   {token_type_t::IDENT, "five"},
                   {token_type_t::WALRUS, ":="},
                   {token_type_t::MULTILINE_STRING, "Multiline stringing"},
                   {token_type_t::SEMICOLON, ";"},
                   {token_type_t::VAR, "var"},
                   {token_type_t::IDENT, "ten"},
                   {token_type_t::WALRUS, ":="},
                   {token_type_t::MULTILINE_STRING, "Multiline stringing\n\\\\Continuation"},
                   {token_type_t::SEMICOLON, ";"},
                   {token_type_t::CONSTANT, "const"},
                   {token_type_t::IDENT, "one"},
                   {token_type_t::WALRUS, ":="},
                   {token_type_t::MULTILINE_STRING, "Nesting \" \' \\ [] const var\n\\\\"},
                   {token_type_t::SEMICOLON, ";"},
               });
}

TEST_CASE("Lexing pointers and references") {
    test_lexer("& &mut * ^ ^mut nullptr",
               {
                   {token_type_t::BW_AND, "&"},
                   {token_type_t::AND_MUT, "&mut"},
                   {token_type_t::STAR, "*"},
                   {token_type_t::CARET, "^"},
                   {token_type_t::CARET_MUT, "^mut"},
                   {token_type_t::NULLPTR, "nullptr"},
               });
}

TEST_CASE("Lexing compiler builtins & Lexer resetting") {
    std::vector<token_type_t> all_builtins;
    all_builtins.insert(all_builtins.end(),
                        syntax::builtins::ALL_TOKEN_TYPES.begin(),
                        syntax::builtins::ALL_TOKEN_TYPES.end());
    all_builtins.insert(all_builtins.end(),
                        syntax::builtins::SPECIAL_FORM_TOKEN_TYPES.begin(),
                        syntax::builtins::SPECIAL_FORM_TOKEN_TYPES.end());
    const auto expecteds{
        std::views::transform(all_builtins, [](const auto& tt) -> syntax::builtin_t {
            return {*syntax::get_builtin_opt(tt), tt};
        })};

    std::string input;
    std::ranges::for_each(expecteds, [&input](const auto& lexeme) -> void {
        input.append(lexeme.name);
        input.push_back(' ');
    });
    syntax::lexer l{input};

    for (const auto& [expected_slice, expected_tt] : expecteds) {
        const auto token{l.advance()};
        CHECK(expected_tt == token.type);
        CHECK(expected_slice == token.slice);
    }
}

TEST_CASE("Lexing illegal builtin") {
    const std::string_view input{"@run"};
    syntax::lexer          l{input};
    const auto             illegal{l.advance()};
    CHECK(token_type_t::ILLEGAL == illegal.type);
    CHECK(input == illegal.slice);
}

} // namespace ghoti::tests
