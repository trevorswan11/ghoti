#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "support/diagnostic.hh"
#include "support/diagnostic_renderer.hh"
#include "support/source_file.hh"

namespace ghoti::tests {

namespace {

enum class render_error : u8 {
    TYPE_MISMATCH,
    UNKNOWN_NAME,
};

using test_diagnostic = diagnostic<render_error>;

const std::filesystem::path MAIN_PATH{"src/a.gh"};
const std::filesystem::path OTHER_PATH{"src/b.gh"};

[[nodiscard]] auto span(usize line, usize start, usize end) -> source_span {
    return {{line, start}, {line, end}};
}

[[nodiscard]] auto at(const std::filesystem::path& path, source_span s) -> located_span {
    return {path, s};
}

// Renders without color, resolving `OTHER_PATH` to `other` when given
[[nodiscard]] auto render(const test_diagnostic&           diag,
                          stdx::option<const source_file&> main,
                          stdx::option<const source_file&> other   = stdx::none,
                          render_options                   options = {}) -> std::string {
    const source_resolver resolve{
        [&other](const std::filesystem::path& path) -> stdx::option<render_source> {
            if (!other || path != OTHER_PATH) { return stdx::none; }
            return render_source{OTHER_PATH, OTHER_PATH.generic_string(), other.raw()};
        }};
    stdx::option<render_source> primary;
    if (main) { primary.emplace(MAIN_PATH, MAIN_PATH.generic_string(), main.raw()); }
    std::ostringstream os;
    options.color = false;
    render_diagnostic(os, diag.to_formattable(), primary, resolve, options);
    return os.str();
}

} // namespace

TEST_CASE("A point diagnostic draws one caret") {
    const source_file     file{std::string_view{"let y = ;\n"}};
    const test_diagnostic diag{
        "Expected an expression", render_error::UNKNOWN_NAME, source_location{0, 8}};
    CHECK(render(diag, file) == R"(error[UNKNOWN_NAME]: Expected an expression
  --> src/a.gh:1:9
   |
 1 | let y = ;
   |         ^
)");
}

TEST_CASE("A span underlines its whole extent with its label inline") {
    const source_file file{std::string_view{"const x: i32 = true;\n"}};
    test_diagnostic   diag{"Type mismatch", render_error::TYPE_MISMATCH, span(0, 15, 19)};
    diag.with_primary_label("this is 'bool'");
    CHECK(render(diag, file) == R"(error[TYPE_MISMATCH]: Type mismatch
  --> src/a.gh:1:16
   |
 1 | const x: i32 = true;
   |                ^^^^ this is 'bool'
)");
}

TEST_CASE("A secondary label to the left hangs below on a pipe") {
    const source_file file{std::string_view{
        "const id = fn(comptime T: type, x: u64): usize { return @sizeOf(T) + x - x; };\n"}};
    test_diagnostic   diag{"Return value of type 'u64' is not assignable to function return type "
                           "'usize'",
                           render_error::TYPE_MISMATCH,
                           span(0, 56, 74)};
    diag.with_primary_label("this is 'u64'")
        .with_label(at(MAIN_PATH, span(0, 41, 46)), "return type declared here")
        .with_note("in instance `id(u64)`, required by src/a.gh:4:26");
    CHECK(
        render(diag, file) ==
        R"(error[TYPE_MISMATCH]: Return value of type 'u64' is not assignable to function return type 'usize'
  --> src/a.gh:1:57
   |
 1 | const id = fn(comptime T: type, x: u64): usize { return @sizeOf(T) + x - x; };
   |                                          -----          ^^^^^^^^^^^^^^^^^^ this is 'u64'
   |                                          |
   |                                          return type declared here
   |
   = note: in instance `id(u64)`, required by src/a.gh:4:26
)");
}

TEST_CASE("Several hanging labels nest from right to left") {
    const source_file file{std::string_view{"f(a, b, c);\n"}};
    test_diagnostic   diag{"Bad call", render_error::TYPE_MISMATCH, span(0, 8, 9)};
    diag.with_primary_label("third")
        .with_label(at(MAIN_PATH, span(0, 2, 3)), "first")
        .with_label(at(MAIN_PATH, span(0, 5, 6)), "second");
    CHECK(render(diag, file) == R"(error[TYPE_MISMATCH]: Bad call
  --> src/a.gh:1:9
   |
 1 | f(a, b, c);
   |   -  -  ^ third
   |   |  |
   |   |  second
   |   first
)");
}

TEST_CASE("Labels on other lines are shown in line order with gaps elided") {
    const source_file file{std::string_view{"a\nb\nc\nd\ne\nf\n"}};
    test_diagnostic   diag{"Gap", render_error::UNKNOWN_NAME, span(5, 0, 1)};
    diag.with_label(at(MAIN_PATH, span(0, 0, 1)), "far above")
        .with_label(at(MAIN_PATH, span(3, 0, 1)), "one line above the gap fill");
    CHECK(render(diag, file) == R"(error[UNKNOWN_NAME]: Gap
  --> src/a.gh:6:1
   |
 1 | a
   | - far above
...
 4 | d
   | - one line above the gap fill
 5 | e
 6 | f
   | ^
)");
}

TEST_CASE("A multi-line span starting at the indent uses a slash gutter") {
    const source_file file{std::string_view{"const f = fn(): i32 {\n    return 1;\n};\n"}};
    test_diagnostic   diag{"Bad body", render_error::TYPE_MISMATCH, source_span{{0, 0}, {2, 1}}};
    diag.with_primary_label("this function");
    CHECK(render(diag, file) == R"(error[TYPE_MISMATCH]: Bad body
  --> src/a.gh:1:1
   |
 1 | / const f = fn(): i32 {
 2 | |     return 1;
 3 | | };
   | |_^ this function
)");
}

TEST_CASE("A multi-line span starting mid-line draws an underscore lead") {
    const source_file file{std::string_view{"let x = foo(\n    a,\n);\n"}};
    test_diagnostic   diag{"Bad call", render_error::TYPE_MISMATCH, source_span{{0, 8}, {2, 1}}};
    diag.with_primary_label("this call");
    CHECK(render(diag, file) == R"(error[TYPE_MISMATCH]: Bad call
  --> src/a.gh:1:9
   |
 1 |   let x = foo(
   |  _________^
 2 | |     a,
 3 | | );
   | |_^ this call
)");
}

TEST_CASE("A span ending at the start of a line stops at the previous line's end") {
    const source_file     file{std::string_view{"abc\ndef\n"}};
    const test_diagnostic diag{
        "Whole line", render_error::UNKNOWN_NAME, source_span{{0, 0}, {1, 0}}};
    CHECK(render(diag, file) == R"(error[UNKNOWN_NAME]: Whole line
  --> src/a.gh:1:1
   |
 1 | abc
   | ^^^
)");
}

TEST_CASE("Tabs expand to four columns and wide characters take two") {
    const source_file     file{std::string_view{"\tlet 名前 = x;\n"}};
    const test_diagnostic diag{"Unknown name", render_error::UNKNOWN_NAME, span(0, 14, 15)};
    CHECK(render(diag, file) == R"(error[UNKNOWN_NAME]: Unknown name
  --> src/a.gh:1:15
   |
 1 |     let 名前 = x;
   |                ^
)");
}

TEST_CASE("A long line is trimmed around its span") {
    const std::string     line{std::string(150, 'a') + " target " + std::string(150, 'b')};
    const source_file     file{line + "\n"};
    const test_diagnostic diag{"Far right", render_error::UNKNOWN_NAME, span(0, 151, 157)};
    const auto            rendered{render(diag, file, stdx::none, {.max_line_width = 40})};
    CHECK(rendered == R"(error[UNKNOWN_NAME]: Far right
  --> src/a.gh:1:152
   |
 1 | …aaaaaaa target bbbbbbbbbbbbbbbbbbbbbbbbb…
   |          ^^^^^^
)");
}

TEST_CASE("A span past the end of its line marks the column after it") {
    const source_file     file{std::string_view{"let x = 1\n"}};
    const test_diagnostic diag{"Expected ';'", render_error::UNKNOWN_NAME, source_location{0, 9}};
    CHECK(render(diag, file) == R"(error[UNKNOWN_NAME]: Expected ';'
  --> src/a.gh:1:10
   |
 1 | let x = 1
   |          ^
)");
}

TEST_CASE("A label in another file gets its own section") {
    const source_file main{std::string_view{"g();\n"}};
    const source_file other{std::string_view{"pub const g = fn(x: i32): void {};\n"}};
    test_diagnostic   diag{"Missing argument", render_error::TYPE_MISMATCH, span(0, 0, 3)};
    diag.with_label(at(OTHER_PATH, span(0, 17, 23)), "parameter declared here");
    CHECK(render(diag, main, other) == R"(error[TYPE_MISMATCH]: Missing argument
  --> src/a.gh:1:1
   |
 1 | g();
   | ^^^
  ::: src/b.gh:1:18
   |
 1 | pub const g = fn(x: i32): void {};
   |                  ------ parameter declared here
)");
}

TEST_CASE("A label in a file that can't be read becomes a note") {
    const source_file main{std::string_view{"g();\n"}};
    test_diagnostic   diag{"Missing argument", render_error::TYPE_MISMATCH, span(0, 0, 3)};
    diag.with_label(at(OTHER_PATH, span(0, 17, 23)), "parameter declared here");
    CHECK(render(diag, main) == R"(error[TYPE_MISMATCH]: Missing argument
  --> src/a.gh:1:1
   |
 1 | g();
   | ^^^
   |
   = note: parameter declared here (src/b.gh:1:18)
)");
}

TEST_CASE("A located note gets its own snippet") {
    const source_file main{std::string_view{"const a = b;\nconst b = a;\n"}};
    test_diagnostic   diag{"Cycle", render_error::UNKNOWN_NAME, span(0, 10, 11)};
    diag.with_note("'b' depends on 'a' here", at(MAIN_PATH, span(1, 10, 11)));
    CHECK(render(diag, main) == R"(error[UNKNOWN_NAME]: Cycle
  --> src/a.gh:1:11
   |
 1 | const a = b;
   |           ^
note: 'b' depends on 'a' here
  --> src/a.gh:2:11
   |
 2 | const b = a;
   |           -
)");
}

TEST_CASE("A warning without a location prints its file alone") {
    test_diagnostic diag{"Something is off", render_error::UNKNOWN_NAME};
    diag.set_level(diagnostic_level::WARNING);
    const source_file file{std::string_view{"x\n"}};
    CHECK(render(diag, file) == R"(warning[UNKNOWN_NAME]: Something is off
  --> src/a.gh
)");
}

TEST_CASE("A diagnostic without a source prints its header and notes") {
    test_diagnostic diag{"No file", render_error::UNKNOWN_NAME};
    diag.with_note("checked late");
    CHECK(render(diag, stdx::none) == R"(error[UNKNOWN_NAME]: No file
   = note: checked late
)");
}

TEST_CASE("A location past the end of the file keeps the header") {
    const source_file     file{std::string_view{"x\n"}};
    const test_diagnostic diag{"Past the end", render_error::UNKNOWN_NAME, source_location{7, 0}};
    CHECK(render(diag, file) == R"(error[UNKNOWN_NAME]: Past the end
  --> src/a.gh:8:1
   |
)");
}

TEST_CASE("The gutter widens for line numbers with more digits") {
    std::string text;
    for (usize i{0}; i < 12; ++i) { text += "x\n"; }
    const source_file     file{text};
    const test_diagnostic diag{"Late", render_error::UNKNOWN_NAME, source_location{11, 0}};
    CHECK(render(diag, file) == R"(error[UNKNOWN_NAME]: Late
   --> src/a.gh:12:1
    |
 12 | x
    | ^
)");
}

TEST_CASE("Color is applied only when asked") {
    const source_file     file{std::string_view{"x\n"}};
    const test_diagnostic diag{"Colored", render_error::UNKNOWN_NAME, source_location{0, 0}};
    std::ostringstream    os;
    render_diagnostic(os,
                      diag.to_formattable(),
                      render_source{MAIN_PATH, MAIN_PATH.generic_string(), &file},
                      {},
                      {.color = true});
    CHECK(os.str().contains("\x1b["));
    CHECK_FALSE(render(diag, file).contains("\x1b["));
}

} // namespace ghoti::tests
