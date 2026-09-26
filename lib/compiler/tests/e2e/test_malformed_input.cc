#include <string>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("unary plus is an identity on numeric operands") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: i32 = 2;
            return +a + +3;
        };
    )") == 5);

    CHECK(helpers::raised("const x := +true;", sema::error::OPERATOR_TYPE_MISMATCH));
}

TEST_CASE("a closed range is not a first-class value") {
    CHECK(helpers::raised(R"(
        const f := fn(): void { const r := 1..5; _ = r; };
    )",
                          sema::error::ILLEGAL_OPEN_RANGE));
}

TEST_CASE("an unsuffixed integer literal typed `constexpr_float` folds as a float") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const b: @TypeOf(0.0) = 1;
            if (b != 1.5f64) { return 7; }
            return 0;
        };
    )") == 7);
}

TEST_CASE("a type argument for an `auto` value parameter is rejected, not miscompiled") {
    CHECK(helpers::raised(R"(
        const f := fn(v: auto): u8 { return 1; };
        const g := fn(): u8 { return f(u16); };
    )",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("`@bitCast` rejects operands of different sizes") {
    CHECK(helpers::raised("const x := @bitCast(i32, @as(i64, 1));", sema::error::TYPE_MISMATCH));
}

TEST_CASE("pointer arithmetic needs a sized pointee") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            var p: ^opaque = undefined;
            const q := p + 1;
            _ = q;
            return 0;
        };
    )");
}

TEST_CASE("range bounds must be integers") {
    CHECK(helpers::raised(R"(
        const f := fn(): void { for (0.5..2.5) |i| { _ = i; } };
    )",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("constant shifts by a negative or oversized amount are rejected") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 { const x: i32 = 1; return x << -1; };
    )");
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 { return 1 << 100000; };
    )");
}

TEST_CASE("enum member values must fit the underlying type and be distinct") {
    CHECK(helpers::raised("const E := enum : u8 { a = 300 };", sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised("const E := enum : u8 { a = -1 };", sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised("const E := enum : u8 { a = 254, b, c };", sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised("const E := enum { a = 1, b = 1 };", sema::error::TYPE_MISMATCH));
}

TEST_CASE("an unvalued enum member continues from its predecessor") {
    CHECK(helpers::compile_and_run(R"(
        const E := enum { a, b = 5, c };
        pub const main := fn(): i32 {
            const e: E = .c;
            return @intCast(i32, @backingInt(e)) + match (e) { .a => 0, .b => 0, .c => 1, };
        };
    )") == 7);
}

TEST_CASE("assignments need an assignable left side") {
    CHECK(helpers::raised("const f := fn(): void { 5 = 6; };", sema::error::TYPE_MISMATCH));
}

TEST_CASE("a value cannot be used as a type") {
    CHECK(helpers::raised("const x := 5; const f := fn(): void { var y: x = 1; _ = y; };",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("`impl X for T` requires X to be an interface") {
    CHECK(helpers::raised("const S := struct {}; impl i32 for S {};", sema::error::TYPE_MISMATCH));
}

TEST_CASE("array lengths must be non-negative and addressable") {
    CHECK(helpers::raised("const f := fn(): void { var a: [-1]u8 = undefined; _ = a; };",
                          sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised(
        "const f := fn(): void { var a: [18446744073709551615]u8 = undefined; _ = a; };",
        sema::error::TYPE_MISMATCH));
}

TEST_CASE("runaway generic type construction is reported, not a stack overflow") {
    helpers::expect_compile_error(R"(
        const f := fn(T: type): type { return f([]T); };
        const X := f(i32);
        pub const main := fn(): i32 { var x: X = undefined; _ = x; return 0; };
    )");
}

TEST_CASE("a local that is read before its declaration is rejected") {
    CHECK(helpers::raised(R"(
        const f := fn(): i32 { var b: i32 = a + 1; var a: i32 = 7; return b; };
    )",
                          sema::error::ILLEGAL_FIELD_ORDER_DEPENDENCY));
}

TEST_CASE("a local function can call an earlier local function") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const g := fn(): i32 { return 4; };
            const f := fn(): i32 { return g() + 1; };
            return f();
        };
    )") == 5);
}

TEST_CASE("a very long constant string literal compiles") {
    std::string src{"pub const main := fn(): i32 { const s := \""};
    src.append(100'000, 'a');
    src += "\"; return @intCast(i32, s.len % 256); };";
    CHECK(helpers::compile_and_run(src) == 100'000 % 256);
}

} // namespace ghoti::tests
