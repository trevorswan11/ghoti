#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <stdx/types.hh>

#include "catch2/catch_message.hpp"
#include "compiler/syntax/error.hh"
#include "compiler/syntax/keywords.hh"
#include "helpers/ast.hh"
#include "helpers/codegen.hh"
#include "helpers/common.hh"
#include "helpers/formatter.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("\\u{...} and \\xHH escapes encode what they name") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            let smile := "\u{1F600}";
            if (smile.len != 4 or smile[0] != 0xF0 or smile[3] != 0x80) { return 1; }
            let accent := "caf\u{E9}";
            if (accent.len != 5 or accent[3] != 0xC3 or accent[4] != 0xA9) { return 2; }
            // A byte escape is one raw byte, even one that isn't valid UTF-8 on its own
            let bytes := "\xFF\x00\x7f";
            if (bytes.len != 3 or bytes[0] != 255 or bytes[1] != 0 or bytes[2] != 127) {
                return 3;
            }
            let letters := "\u{41}\u{0042}";
            if (letters.len != 2 or letters[1] != 'B') { return 4; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("a character literal is its code point, defaulting to u21") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            let mut letter := 'a';
            if (@TypeOf(letter) != u21 or letter != 97) { return 1; }
            let byte: u8 = 'b';
            if (byte != 98) { return 2; }
            let bytes := "xyz";
            if (bytes[1] != 'y') { return 3; }
            if ('é' != 0xE9 or '😀' != 0x1F600) { return 4; }
            if ('\u{1F600}' != 0x1F600 or '\x7F' != 127 or '\xFF' != 255 or '\n' != 10) {
                return 5;
            }
            let mut wide := '😀';
            if (@TypeOf(wide) != u21) { return 6; }
            let as_u32: u32 = '😀';
            if (as_u32 != 0x1F600) { return 7; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("a character literal must hold exactly one code point that fits its type") {
    helpers::test_parser_fail(
        "const c := 'ab';",
        syntax::diagnostic{"Character literal must contain exactly one code point; found 2",
                           syntax::error::INVALID_CHARACTER_LITERAL,
                           std::pair{0UZ, 11UZ}});
    // A family emoji is five code points joined together
    helpers::test_parser_fail(
        "const c := '\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7';",
        syntax::diagnostic{"Character literal must contain exactly one code point; found 5",
                           syntax::error::INVALID_CHARACTER_LITERAL,
                           std::pair{0UZ, 11UZ}});
    helpers::expect_compile_error(
        "pub const main := fn(): i32 { let c: u8 = '\xF0\x9F\x98\x80'; return 0; };");
}

TEST_CASE("a malformed escape is reported at the escape") {
    const auto at{[](std::string_view source, std::string_view message, usize column) {
        helpers::test_parser_fail(source,
                                  syntax::diagnostic{std::string{message},
                                                     syntax::error::UNKNOWN_CHARACTER_ESCAPE,
                                                     std::pair{0UZ, column}});
    }};
    at(R"(const s := "ab\u{110000}";)",
       "U+110000 is past the last Unicode code point, U+10FFFF",
       14);
    at(R"(const s := "ab\u{D800}";)", "U+D800 is a surrogate, not a Unicode scalar value", 14);
    at(R"(const s := "\u41";)", "Expected '{' after '\\u'", 12);
    at(R"(const s := "x\u{}";)", "Expected hex digits in '\\u{...}'", 13);
    at(R"(const s := "\u{1234567}";)", "'\\u{...}' takes 1 to 6 hex digits", 12);
    at(R"(const s := "\u{12";)", "Expected '}' to close '\\u{...}'", 12);
    at(R"(const s := "\xG0";)", "Expected two hex digits after '\\x'", 12);
    at(R"(const c := '\u{110000}';)", "U+110000 is past the last Unicode code point, U+10FFFF", 12);
    at(R"(const @"a\q" := 1;)", "Unknown escape sequence '\\q'", 9);
}

TEST_CASE("a raw identifier is the same name however it is spelled") {
    CHECK(helpers::compile_and_run(R"(
        const @"😀" := 20;
        const @"caf\u{E9}" := 22;
        pub const main := fn(): i32 { return @"\u{1F600}" + @"café"; };
    )") == 42);
    // No normalization: a precomposed and a decomposed é are different names
    helpers::expect_compile_error(R"(
        const @"caf\u{E9}" := 1;
        pub const main := fn(): i32 { return @"cafe\u{301}"; };
    )");
    helpers::test_parser_fail("const @\"\xFF\" := 1;",
                              syntax::diagnostic{"Raw identifier is not valid UTF-8",
                                                 syntax::error::INVALID_UTF8,
                                                 std::pair{0UZ, 6UZ}});
    CHECK(syntax::identifier_needs_raw("\xF0\x9F\x98\x80"));
    CHECK(syntax::identifier_needs_raw("caf\xC3\xA9"));
}

TEST_CASE("reflection names a raw identifier by its decoded name") {
    CHECK(helpers::compile_and_run(R"(
        const Mood := enum { @"😀", @"caf\u{E9}" };
        pub const main := fn(): i32 {
            let smile := @tagName(Mood.@"😀");
            if (smile.len != 4 or smile[0] != 0xF0) { return 1; }
            let cafe := @tagName(Mood.@"café");
            if (cafe.len != 5 or cafe[4] != 0xA9) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("a non-ASCII export links under its UTF-8 name on every object format") {
    for (const std::string_view triple : {"x86_64-unknown-linux-gnu",
                                          "x86_64-pc-windows-msvc",
                                          "aarch64-apple-macos",
                                          "wasm32-unknown-unknown"}) {
        INFO(triple);
        llvm::LLVMContext context;
        auto [ctx, idx]{helpers::type_check_for_target(R"(
            pub const @"😀" := fn(): i32 { return 1; };
            const @"内部" := fn(): i32 { return 2; };
            pub const both := fn(): i32 { return @"😀"() + @"内部"(); };
        )",
                                                       triple)};
        auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
        CHECK(llvm_mod->getFunction("\xF0\x9F\x98\x80") != nullptr);
    }
    CHECK(helpers::compile_and_run(R"(
        const @"😀" := fn(): i32 { return 40; };
        const @"内部" := fn(): i32 { return 2; };
        pub const main := fn(): i32 { return @"😀"() + @"内部"(); };
    )") == 42);
}

TEST_CASE("the formatter keeps non-ASCII names raw and measures them in columns") {
    helpers::round_trips("const @\"\xF0\x9F\x98\x80\" := @\"caf\xC3\xA9\";\n");
    CHECK(helpers::format_source("const @\"\xF0\x9F\x98\x80\" := 1;") ==
          "const @\"\xF0\x9F\x98\x80\" := 1;\n");
    // Thirty CJK characters are ninety bytes but sixty columns: the call fits in 80 columns
    std::string wide;
    for (usize i{0}; i < 30; ++i) { wide += "\xE4\xBD\xA0"; }
    const auto source{"const x := f(\"" + wide + "\", 1);\n"};
    CHECK(helpers::format_source(source, 80) == source);
    CHECK(helpers::format_source(helpers::format_source(source, 80), 80) == source);
}

} // namespace ghoti::tests
