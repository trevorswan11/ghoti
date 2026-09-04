#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("a non-packable field is rejected in a bit-packed packed struct") {
    CHECK(helpers::raised("const S := packed struct { a: []i32 };",
                          sema::error::ILLEGAL_PACKED_FIELD));
    CHECK(helpers::raised("const S := packed struct { a: fn(): void };",
                          sema::error::ILLEGAL_PACKED_FIELD));
}

TEST_CASE("alignas on a bit-packed packed struct field is rejected") {
    CHECK(helpers::raised("const S := packed struct { @alignas(4) a: u8, b: u8 };",
                          sema::error::ILLEGAL_PACKED_FIELD));
}

TEST_CASE("a bit-packed packed struct wider than 128 bits is rejected") {
    CHECK(helpers::raised("const S := packed struct { a: u100, b: u100 };",
                          sema::error::ILLEGAL_PACKED_FIELD));
}

TEST_CASE("taking the address of a bit-packed packed struct field is rejected") {
    CHECK(helpers::raised(R"(
        const S := packed struct { a: u3, b: u5 };
        pub const main := fn(): i32 {
            var s: S = .{ .a = 1, .b = 2 };
            const p := &s.a;
            return 0;
        };
    )",
                          sema::error::ILLEGAL_PACKED_FIELD_ADDRESS));
}

TEST_CASE("packable fields are accepted in a bit-packed packed struct") {
    helpers::resolve_and_check(R"(
        const C := enum : u2 { A = 0, B = 1 };
        const Inner := packed struct { x: u4, y: u4 };
        const S := packed struct { on: bool, c: C, n: u5, inner: Inner, p: ^i32 };
    )");
}

// An `extern packed struct` keeps C layout, so `alignas` and larger widths stay legal.
TEST_CASE("extern packed struct is not bit-packed and keeps its relaxed rules") {
    helpers::resolve_and_check(
        "const S := extern packed struct { @alignas(4) a: i32, b: u8, c: i32 };");
}

TEST_CASE("packed union accepts packable fields and rejects the rest") {
    helpers::resolve_and_check("const U := packed union { a: u8, b: u3, f: f16, p: ^i32 };");
    CHECK(helpers::raised("const U := packed union { a: u8, b: []i32 };",
                          sema::error::ILLEGAL_PACKED_FIELD));
    CHECK(helpers::raised("const U := packed union { @alignas(4) a: u8, b: u3 };",
                          sema::error::ILLEGAL_PACKED_FIELD));
    CHECK(helpers::raised("const U := packed union { a: u200, b: u8 };",
                          sema::error::ILLEGAL_PACKED_FIELD));
}

TEST_CASE("taking the address of a bit-packed union field is rejected") {
    CHECK(helpers::raised(R"(
        const U := packed union { a: u8, b: u3 };
        pub const main := fn(): i32 {
            var u: U = .{ .a = 1 };
            const p := &u.a;
            return 0;
        };
    )",
                          sema::error::ILLEGAL_PACKED_FIELD_ADDRESS));
}

} // namespace ghoti::tests
