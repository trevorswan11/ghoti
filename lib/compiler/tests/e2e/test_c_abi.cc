#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <stdx/types.hh>

#include "ghoti/test_fixtures.h"
#include "helpers/codegen.hh"

namespace ghoti::tests {

namespace {

// Runs `source` linked against the C fixtures in `tests/ffi/cabi.c`
[[nodiscard]] auto run_against_c(std::string_view source) -> u32 {
    const std::filesystem::path        fixture{GHOTI_CABI_FIXTURE};
    std::vector<std::filesystem::path> library_dirs{fixture.parent_path()};
    auto                               linker_opts{helpers::default_linker_options()};
    linker_opts.library_paths = library_dirs;
    return helpers::compile_and_run(source, {}, linker_opts);
}

[[nodiscard]] auto extern_decl(std::string_view name, std::string_view signature) -> std::string {
    return fmt::format(
        "extern(\"cabi_fixture\", \"{0}\") const {0}: extern fn{1};\n", name, signature);
}

// One aggregate mirrored from `cabi.c`, with what the C side treats as its canonical value
struct aggregate_case {
    std::string_view name;
    std::string_view definition;
    std::string_view canonical;
    std::string_view bumped;
    std::string_view bump_body; // builds the bumped value from `v`
    std::string_view equality;  // `{0}` is the value and `{1}` the expected one
};

[[nodiscard]] auto suite_program(const aggregate_case& c) -> std::string {
    std::string source{fmt::format("const {} = {};\n", c.name, c.definition)};
    source += extern_decl(fmt::format("check_{}", c.name), fmt::format("(v: {}): i32", c.name));
    source += extern_decl(fmt::format("make_{}", c.name), fmt::format("(): {}", c.name));
    source += extern_decl(fmt::format("bump_{}", c.name), fmt::format("(v: {0}): {0}", c.name));
    source += extern_decl(fmt::format("call_{}", c.name),
                          fmt::format("(cb: extern fn(v: {0}): {0}): i32", c.name));

    const auto expect{[&](std::string_view value, std::string_view expected) {
        return fmt::format(fmt::runtime(c.equality), value, expected);
    }};
    source += fmt::format(R"(
const bump_in_ghoti = fn(v: {0}) callconv(.c): {0} {{ return {1}; }};

pub const main = fn(): i32 {{
    const canonical: {0} = {2};
    const bumped: {0} = {3};
    let mut failed: i32 = 0;
    if (check_{0}(canonical) != 1) {{ failed += 1; }}
    let made = make_{0}();
    if (!({4})) {{ failed += 2; }}
    let echoed = bump_{0}(canonical);
    if (!({5})) {{ failed += 4; }}
    if (call_{0}(bump_in_ghoti) != 1) {{ failed += 8; }}
    return failed;
}};
)",
                          c.name,
                          c.bump_body,
                          c.canonical,
                          c.bumped,
                          expect("made", "canonical"),
                          expect("echoed", "bumped"));
    return source;
}

constexpr std::string_view EQ_AB{"{0}.a == {1}.a and {0}.b == {1}.b"};
constexpr std::string_view EQ_ABC{"{0}.a == {1}.a and {0}.b == {1}.b and {0}.c == {1}.c"};
constexpr std::string_view EQ_ABCD{
    "{0}.a == {1}.a and {0}.b == {1}.b and {0}.c == {1}.c and {0}.d == {1}.d"};
constexpr std::string_view EQ_XY{"{0}.x == {1}.x and {0}.y == {1}.y"};
constexpr std::string_view EQ_XYZ{"{0}.x == {1}.x and {0}.y == {1}.y and {0}.z == {1}.z"};
constexpr std::string_view EQ_XYZW{
    "{0}.x == {1}.x and {0}.y == {1}.y and {0}.z == {1}.z and {0}.w == {1}.w"};
constexpr std::string_view BUMP_AB{".{ .a = v.a + 1, .b = v.b + 1 }"};
constexpr std::string_view BUMP_ABC{".{ .a = v.a + 1, .b = v.b + 1, .c = v.c + 1 }"};
constexpr std::string_view BUMP_ABCD{".{ .a = v.a + 1, .b = v.b + 1, .c = v.c + 1, .d = v.d + 1 }"};
constexpr std::string_view BUMP_XY{".{ .x = v.x + 1.0, .y = v.y + 1.0 }"};
constexpr std::string_view BUMP_XYZ{".{ .x = v.x + 1.0, .y = v.y + 1.0, .z = v.z + 1.0 }"};
constexpr std::string_view BUMP_XYZW{
    ".{ .x = v.x + 1.0, .y = v.y + 1.0, .z = v.z + 1.0, .w = v.w + 1.0 }"};

// clang-format off
const std::vector<aggregate_case> AGGREGATES{
    {"S1", "extern struct { a: u8 }", ".{ .a = 11 }", ".{ .a = 12 }", ".{ .a = v.a + 1 }",
     "{0}.a == {1}.a"},
    {"S2", "extern struct { a: u8, b: u8 }", ".{ .a = 11, .b = 22 }", ".{ .a = 12, .b = 23 }",
     BUMP_AB, EQ_AB},
    {"S3", "extern struct { a: u8, b: u8, c: u8 }", ".{ .a = 11, .b = 22, .c = 33 }",
     ".{ .a = 12, .b = 23, .c = 34 }", BUMP_ABC, EQ_ABC},
    {"S4", "extern struct { a: i16, b: i16 }", ".{ .a = -1111, .b = 2222 }",
     ".{ .a = -1110, .b = 2223 }", BUMP_AB, EQ_AB},
    {"S6", "extern struct { a: i16, b: i16, c: i16 }", ".{ .a = -11, .b = 22, .c = -33 }",
     ".{ .a = -10, .b = 23, .c = -32 }", BUMP_ABC, EQ_ABC},
    {"S8", "extern struct { a: i32, b: i32 }", ".{ .a = -100000, .b = 200000 }",
     ".{ .a = -99999, .b = 200001 }", BUMP_AB, EQ_AB},
    {"S12", "extern struct { a: i32, b: i32, c: i32 }", ".{ .a = 1, .b = -2, .c = 3 }",
     ".{ .a = 2, .b = -1, .c = 4 }", BUMP_ABC, EQ_ABC},
    {"S16", "extern struct { a: i64, b: i64 }", ".{ .a = -5000000000, .b = 6000000000 }",
     ".{ .a = -4999999999, .b = 6000000001 }", BUMP_AB, EQ_AB},
    {"S24", "extern struct { a: i64, b: i64, c: i64 }",
     ".{ .a = 7000000000, .b = -8000000000, .c = 9000000000 }",
     ".{ .a = 7000000001, .b = -7999999999, .c = 9000000001 }", BUMP_ABC, EQ_ABC},
    {"S32", "extern struct { a: i64, b: i64, c: i64, d: i64 }",
     ".{ .a = 1, .b = -2, .c = 3, .d = -4 }", ".{ .a = 2, .b = -1, .c = 4, .d = -3 }",
     BUMP_ABCD, EQ_ABCD},
    {"A5", "extern struct { bytes: [5]u8 }", ".{ .bytes = .{ 1, 2, 3, 4, 5 } }",
     ".{ .bytes = .{ 2, 3, 4, 5, 6 } }",
     ".{ .bytes = .{ v.bytes[0] + 1, v.bytes[1] + 1, v.bytes[2] + 1, v.bytes[3] + 1, "
     "v.bytes[4] + 1 } }",
     "{0}.bytes[0] == {1}.bytes[0] and {0}.bytes[1] == {1}.bytes[1] and "
     "{0}.bytes[2] == {1}.bytes[2] and {0}.bytes[3] == {1}.bytes[3] and "
     "{0}.bytes[4] == {1}.bytes[4]"},
    {"F2", "extern struct { x: f32, y: f32 }", ".{ .x = 1.5, .y = -2.5 }",
     ".{ .x = 2.5, .y = -1.5 }", BUMP_XY, EQ_XY},
    {"F3", "extern struct { x: f32, y: f32, z: f32 }", ".{ .x = 1.5, .y = -2.5, .z = 3.5 }",
     ".{ .x = 2.5, .y = -1.5, .z = 4.5 }", BUMP_XYZ, EQ_XYZ},
    {"F4", "extern struct { x: f32, y: f32, z: f32, w: f32 }",
     ".{ .x = 1.5, .y = -2.5, .z = 3.5, .w = -4.5 }",
     ".{ .x = 2.5, .y = -1.5, .z = 4.5, .w = -3.5 }", BUMP_XYZW, EQ_XYZW},
    {"D2", "extern struct { x: f64, y: f64 }", ".{ .x = 1.25, .y = -2.25 }",
     ".{ .x = 2.25, .y = -1.25 }", BUMP_XY, EQ_XY},
    {"D3", "extern struct { x: f64, y: f64, z: f64 }", ".{ .x = 1.25, .y = -2.25, .z = 3.25 }",
     ".{ .x = 2.25, .y = -1.25, .z = 4.25 }", BUMP_XYZ, EQ_XYZ},
    {"D4", "extern struct { x: f64, y: f64, z: f64, w: f64 }",
     ".{ .x = 1.25, .y = -2.25, .z = 3.25, .w = -4.25 }",
     ".{ .x = 2.25, .y = -1.25, .z = 4.25, .w = -3.25 }", BUMP_XYZW, EQ_XYZW},
    {"MixIF", "extern struct { i: i32, f: f32 }", ".{ .i = -7, .f = 8.5 }",
     ".{ .i = -6, .f = 9.5 }", ".{ .i = v.i + 1, .f = v.f + 1.0 }",
     "{0}.i == {1}.i and {0}.f == {1}.f"},
    {"MixFII", "extern struct { f: f32, i: i32, j: i32 }", ".{ .f = 8.5, .i = -7, .j = 9 }",
     ".{ .f = 9.5, .i = -6, .j = 10 }", ".{ .f = v.f + 1.0, .i = v.i + 1, .j = v.j + 1 }",
     "{0}.f == {1}.f and {0}.i == {1}.i and {0}.j == {1}.j"},
    {"MixDI", "extern struct { d: f64, i: i64 }", ".{ .d = -3.75, .i = 123456789012 }",
     ".{ .d = -2.75, .i = 123456789013 }", ".{ .d = v.d + 1.0, .i = v.i + 1 }",
     "{0}.d == {1}.d and {0}.i == {1}.i"},
    {"MixID", "extern struct { i: i64, d: f64 }", ".{ .i = 123456789012, .d = -3.75 }",
     ".{ .i = 123456789013, .d = -2.75 }", ".{ .i = v.i + 1, .d = v.d + 1.0 }",
     "{0}.i == {1}.i and {0}.d == {1}.d"},
    {"Nested", "extern struct { inner: extern struct { a: u8, b: u8 }, tail: u16 }",
     ".{ .inner = .{ .a = 11, .b = 22 }, .tail = 33333 }",
     ".{ .inner = .{ .a = 12, .b = 23 }, .tail = 33334 }",
     ".{ .inner = .{ .a = v.inner.a + 1, .b = v.inner.b + 1 }, .tail = v.tail + 1 }",
     "{0}.inner.a == {1}.inner.a and {0}.inner.b == {1}.inner.b and {0}.tail == {1}.tail"},
    {"UIF", "extern union { i: i32, f: f32 }", ".{ .i = 0x12345678 }", ".{ .i = 0x12345679 }",
     ".{ .i = v.i + 1 }", "{0}.i == {1}.i"},
};
// clang-format on

} // namespace

TEST_CASE("Aggregates cross the C ABI by value in both directions") {
    for (const auto& c : AGGREGATES) {
        INFO(c.name << ": bit 1 = ghoti->C arg, 2 = C->ghoti return, 4 = round trip through C, "
                       "8 = C calling a ghoti callback");
        CHECK(run_against_c(suite_program(c)) == 0);
    }
}

TEST_CASE("Aggregates past the by-pointer threshold cross the C ABI") {
    std::string source{"const Big320 = extern struct { values: [40]i64 };\n"};
    source += extern_decl("sum_big", "(big: Big320): i64");
    source += extern_decl("make_big", "(seed: i64): Big320");
    source += R"(
pub const main = fn(): i32 {
    let mut failed: i32 = 0;
    let big = make_big(100);
    if (big.values[0] != 100 or big.values[39] != 139) { failed += 1; }
    let mut expected: i64 = 0;
    for (0..40) |k| { expected += (100 + @as(i64, k)) * (@as(i64, k) + 1); }
    if (sum_big(big) != expected) { failed += 2; }
    return failed;
};
)";
    CHECK(run_against_c(source) == 0);
}

TEST_CASE("Narrow integers and bools cross the C ABI extended") {
    std::string source;
    source += extern_decl("widen_i8", "(v: i8): i32");
    source += extern_decl("widen_u8", "(v: u8): i32");
    source += extern_decl("widen_i16", "(v: i16): i32");
    source += extern_decl("widen_u16", "(v: u16): i32");
    source += extern_decl("widen_bool", "(v: bool): i32");
    source += extern_decl("narrow_i8", "(v: i32): i8");
    source += extern_decl("narrow_u8", "(v: i32): u8");
    source += extern_decl("is_positive", "(v: i32): bool");
    source += R"(
pub const main = fn(): i32 {
    let mut failed: i32 = 0;
    let base: i32 = 1000;
    if (widen_i8(@truncate(i8, base - 1005)) != -5) { failed += 1; }
    if (widen_u8(@truncate(u8, base - 750)) != 250) { failed += 2; }
    if (widen_i16(@truncate(i16, base - 31000)) != -30000) { failed += 4; }
    if (widen_u16(@truncate(u16, base + 59000)) != 60000) { failed += 8; }
    if (widen_bool(base > 0) != 1) { failed += 16; }
    if (narrow_i8(base - 1003) != -3) { failed += 32; }
    if (narrow_u8(base - 800) != 200) { failed += 64; }
    if (!is_positive(base)) { failed += 128; }
    return failed;
};
)";
    CHECK(run_against_c(source) == 0);
}

TEST_CASE("Arguments past the registers spill to the stack as C expects") {
    std::string source;
    source += extern_decl("sum_i64", "(a: i64, b: i64): i64");
    source += extern_decl("mul_f64", "(a: f64, b: f32): f64");
    source += extern_decl("many_ints",
                          "(a: i32, b: i64, c: i8, d: i16, e: i32, f: i64, g: i32, h: i64, i: i32, "
                          "j: i64): i64");
    source +=
        extern_decl("many_floats",
                    "(a: f64, b: f32, c: f64, d: f32, e: f64, f: f32, g: f64, h: f32, i: f64, "
                    "j: f32): f64");
    source += "const S16 = extern struct { a: i64, b: i64 };\n";
    source +=
        extern_decl("late_struct", "(a: i64, b: i64, c: i64, d: i64, e: i64, f: i64, s: S16): i32");
    source += R"(
pub const main = fn(): i32 {
    let mut failed: i32 = 0;
    if (sum_i64(4000000000, 5000000000) != 9000000000) { failed += 1; }
    if (mul_f64(1.5, 4.0) != 6.0) { failed += 2; }
    if (many_ints(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) != 385) { failed += 4; }
    if (many_floats(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0) != 385.0) { failed += 8; }
    if (late_struct(1, 2, 3, 4, 5, 6, .{ .a = 7, .b = 8 }) != 1) { failed += 16; }
    return failed;
};
)";
    CHECK(run_against_c(source) == 0);
}

TEST_CASE("C variadic functions read ghoti's arguments") {
    std::string source;
    source += extern_decl("sum_varargs", "(count: i32, ...): i64");
    source += extern_decl("sum_double_varargs", "(count: i32, ...): f64");
    source += R"(
pub const main = fn(): i32 {
    let mut failed: i32 = 0;
    if (sum_varargs(3, @as(i64, 10), @as(i64, -20), @as(i64, 7000000000)) != 6999999990) {
        failed += 1;
    }
    if (sum_double_varargs(3, @as(f64, 1.5), @as(f32, 2.5), @as(f64, -0.5)) != 3.5) {
        failed += 2;
    }
    return failed;
};
)";
    CHECK(run_against_c(source) == 0);
}

TEST_CASE("C calls back into ghoti and reads ghoti-visible C data") {
    std::string source;
    source += extern_decl("call_int_callback",
                          "(cb: extern fn(a: i32, b: i32): i32, a: i32, b: i32): i32");
    source += extern_decl("counter_address", "(): ^mut i32");
    source += R"(
const subtract = fn(a: i32, b: i32) callconv(.c): i32 { return a - b; };

pub const main = fn(): i32 {
    let mut failed: i32 = 0;
    if (call_int_callback(subtract, 50, 8) != 43) { failed += 1; }
    let counter = counter_address();
    if (*counter != 41) { failed += 2; }
    *counter = 7;
    if (*counter_address() != 7) { failed += 4; }
    return failed;
};
)";
    CHECK(run_against_c(source) == 0);
}

} // namespace ghoti::tests
