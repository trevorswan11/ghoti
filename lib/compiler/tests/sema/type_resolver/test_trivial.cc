#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/types.hh>

#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace syms = sema::symbols;

TEST_CASE("Builtin type resolution") {
    const auto check_bi_type = [](std::string_view value, std::string_view expected_name) -> void {
        auto [ctx, idx]{helpers::resolve_and_check(fmt::format("const a := {};", value))};
        const auto [sym, data, type]{ctx->get_type_sym_info<syms::node_t>("a", idx)};
        CHECK(sema::type_kind_display_name(type) == expected_name);
    };

    check_bi_type("1", "constexpr_int"); // unsuffixed: stays constexpr in an un-annotated const
    check_bi_type("1i64", "i64");
    check_bi_type("1z", "isize");
    check_bi_type("1u32", "u32");
    check_bi_type("1u64", "u64");
    check_bi_type("1UZ", "usize");
    check_bi_type("'1'", "u8");
    check_bi_type("true", "bool");
    check_bi_type("{}", "void");
    check_bi_type("undefined", "undefined");
    check_bi_type("unreachable", "noreturn");
    check_bi_type("1.0f32", "f32");
    check_bi_type("1.0", "constexpr_float");
}

TEST_CASE("Nested type resolution") {
    auto [ctx,
          idx]{helpers::resolve_and_check("var a: ^^i32 = undefined; var b: ^^^i32 = undefined;")};

    const auto& i32_ptr = ctx->get_type(sema::type_kind::POINTER, ctx->get_int_type(32, true));
    const auto& i32_ptr_ptr{ctx->get_type(sema::type_kind::POINTER, i32_ptr)};

    const auto [a_sym, a_sym_data, a_type]{ctx->get_type_sym_info<syms::node_t>("a", idx)};
    CHECK(a_type == i32_ptr_ptr);
    const auto [b_sym, b_sym_data, b_type]{ctx->get_type_sym_info<syms::node_t>("b", idx)};
    CHECK(b_type == ctx->get_type(sema::type_kind::POINTER, i32_ptr_ptr));
}

TEST_CASE("Type alias resolution") {
    auto [ctx, idx]{helpers::resolve_and_check(
        "const a := ^bool; var b: a = undefined; var c: &a = undefined;")};
    const auto& bool_ref =
        ctx->get_type(sema::type_kind::POINTER, ctx->get_type(sema::type_kind::BOOL));

    const auto [a_sym, a_sym_data, a_type]{ctx->get_type_sym_info<syms::node_t>("a", idx)};
    CHECK(a_type == bool_ref);
    const auto [b_sym, b_sym_data, b_type]{ctx->get_type_sym_info<syms::node_t>("b", idx)};
    CHECK(b_type == bool_ref);
    const auto [c_sym, c_sym_data, c_type]{ctx->get_type_sym_info<syms::node_t>("c", idx)};
    CHECK(c_type == ctx->get_type(sema::type_kind::REFERENCE, bool_ref));
}

TEST_CASE("`using` can name an ordinary binding") {
    helpers::resolve_and_check("const using := 3; const x: i32 = using;");
}

TEST_CASE("A `const` alias names a value or a type by what its right-hand side denotes") {
    helpers::resolve_and_check("const BASE := 42; const K := BASE; const x: i32 = K;");
    helpers::resolve_and_check("const S := struct { x: i32 }; const T := S; var t: T = undefined;");
    helpers::resolve_and_check("const Byte := u8; const b: Byte = 7;");
}

TEST_CASE("Unary expression resolution") {
    helpers::resolve_and_check("var a: ^i32 = undefined; _ = *a;");
    helpers::resolve_and_check("_ = !1;");
    helpers::resolve_and_check("_ = ~1;");
    helpers::resolve_and_check("_ = -1;");

    helpers::test_resolver_fail(
        "var a: i32 = undefined; _ = *a;",
        sema::diagnostic{"Cannot dereference non-pointer expression; found 'i32'",
                         sema::error::TYPE_MISMATCH,
                         std::pair{0UZ, 28UZ}});
}

TEST_CASE("Undeclared identifier usage") {
    const auto expected_diag = [](usize col) -> sema::diagnostic {
        return {
            "Use of undeclared identifier 'b'",
            sema::error::UNDECLARED_IDENTIFIER,
            std::pair{0UZ, col},
        };
    };

    helpers::test_resolver_fail("const a := b;", expected_diag(11));
    helpers::test_resolver_fail("const a := ^b;", expected_diag(12));
}

TEST_CASE("Value-less extern") { helpers::resolve_and_check("extern var errno: i32;"); }

TEST_CASE("Defer & discard statement resolution") {
    auto [ctx, idx]{helpers::resolve_and_check("fn(): void { defer { var a: i32 = undefined; } }")};
    const auto [sym, _, type]{ctx->get_type_sym_info<syms::node_t>("a", 2)};
    CHECK(type == ctx->get_int_type(32, true));
    helpers::resolve_and_check("_ = 1 + 1;");
}

TEST_CASE("Defer body jump rejection") {
    helpers::test_resolver_fail("fn(): void { defer { return; } }",
                                sema::diagnostic{"cannot 'return' from inside a 'defer' body",
                                                 sema::error::DEFER_BODY_JUMP,
                                                 std::pair{0UZ, 21UZ}});

    helpers::test_resolver_fail("fn(): void { defer { break; } }",
                                sema::diagnostic{"cannot 'break' from inside a 'defer' body",
                                                 sema::error::DEFER_BODY_JUMP,
                                                 std::pair{0UZ, 21UZ}});

    helpers::test_resolver_fail("fn(): void { defer { continue; } }",
                                sema::diagnostic{"cannot 'continue' from inside a 'defer' body",
                                                 sema::error::DEFER_BODY_JUMP,
                                                 std::pair{0UZ, 21UZ}});

    helpers::test_resolver_fail(
        R"(
const R := union { ok: i32, err: i32 };
impl builtin.Unwrappable for R {
    const Output := i32;
    const Residual := i32;
    pub const branch := fn(self): builtin.Flow(i32, i32) {
        return match (self) {
            .ok => |v| builtin.Flow(i32, i32){ .@"continue" = v },
            .err => |e| builtin.Flow(i32, i32){ .@"break" = e },
        };
    };
}
impl builtin.Rewrappable for R {
    const From := i32;
    pub const from_residual := fn(r: i32): @This() { return .{ .err = r }; };
}
const f := fn(): R {
    defer {
        const r := R{ .ok = 1 };
        _ = r?;
    }
    return R{ .ok = 0 };
};
)",
        sema::diagnostic{"cannot use '?' operator inside a 'defer' body",
                         sema::error::DEFER_BODY_JUMP,
                         std::pair{19UZ, 12UZ}});

    SECTION("Loops and local blocks inside defer are allowed to use break and continue") {
        helpers::resolve_and_check(R"(
fn(): void {
    defer {
        while (true) {
            break;
            continue;
        }
        blk: {
            break :blk;
        }
    }
}
)");
    }
}

TEST_CASE("Call resolution edge cases") {
    helpers::resolve_and_check(
        "@sizeOf(blk: { if (1 + 1 == 2) { break :blk i32; } else { break :blk f64; } });");
    helpers::resolve_and_check("@TypeOf([]i32);");
    helpers::test_resolver_fail("const a := b; const c := a();",
                                sema::diagnostic{"Use of undeclared identifier 'b'",
                                                 sema::error::UNDECLARED_IDENTIFIER,
                                                 std::pair{0UZ, 11UZ}});
}

TEST_CASE("Loop resolution") {
    helpers::resolve_and_check("const a := loop { const foo := 42; };");
    helpers::test_resolver_fail(
        "for (23) |_| { var a: i32 = undefined; }",
        sema::diagnostic{"Iterables may only be arrays or slices; found 'constexpr_int'",
                         sema::error::TYPE_MISMATCH,
                         std::pair{0UZ, 5UZ}});
}

TEST_CASE("Duplicate test name") {
    helpers::test_resolver_fail(
        R"(test "TEST ME" { var a: i32 = undefined; } test "TEST ME" { var a: i32 = undefined; })",
        sema::diagnostic{"Duplicate test block named 'TEST ME'; previous declaration here: 1:1",
                         sema::error::DUPLICATE_TEST_NAME,
                         std::pair{0UZ, 43UZ}});
}

TEST_CASE("Illegal initializer targets") {
    const auto expected_diag = [](usize col) -> sema::diagnostic {
        return {"Only struct and union types may be used in initializer expressions; found 'i32'",
                sema::error::TYPE_MISMATCH,
                std::pair{0UZ, col}};
    };

    helpers::test_resolver_fail("i32{};", expected_diag(3));
    helpers::test_resolver_fail("const a: i32 = .{};", expected_diag(16));
}

TEST_CASE("Duplicate top-level symbols resolve without crashing") {
    helpers::test_resolver_fail(
        "pub const x := 5; pub const x := 6;",
        sema::diagnostic{"Redeclaration of symbol 'x'; previous declaration here: 1:11",
                         sema::error::IDENTIFIER_REDECLARATION,
                         std::pair{0UZ, 28UZ}});

    helpers::test_resolver_fail(
        "const X := i32; const X := i64;",
        sema::diagnostic{"Redeclaration of symbol 'X'; previous declaration here: 1:7",
                         sema::error::IDENTIFIER_REDECLARATION,
                         std::pair{0UZ, 22UZ}});
}

TEST_CASE("Dereferenced assignment using non-pointer fails") {
    helpers::test_resolver_fail(
        R"(
        pub const test_fn := fn(x: i32): void {
            const bad := *x;
        };
    )",
        sema::diagnostic{"Cannot dereference non-pointer expression; found 'i32'",
                         sema::error::TYPE_MISMATCH,
                         std::pair{2UZ, 25UZ}});
}

TEST_CASE("Mutable borrow of rvalue is rejected") {
    helpers::test_resolver_fail(
        "pub const test_fn := fn(): void { const p := &mut 42; };",
        sema::diagnostic{"Cannot take a mutable reference to a temporary value",
                         sema::error::ILLEGAL_RVALUE_CAPTURE,
                         std::pair{0UZ, 45UZ}});

    helpers::test_resolver_fail(
        "pub const test_fn := fn(): void { const p := ^mut 42; };",
        sema::diagnostic{"Cannot take a mutable pointer to a temporary value",
                         sema::error::ILLEGAL_RVALUE_CAPTURE,
                         std::pair{0UZ, 45UZ}});
}

TEST_CASE("Method requiring mutable self on rvalue is rejected") {
    helpers::test_resolver_fail(
        R"(
        const Counter := struct {
            val: i32,
            pub const inc := fn(&mut self): void { self.val += 1; };
        };
        pub const test_fn := fn(): void {
            (Counter{ .val = 0 }).inc();
        };
    )",
        sema::diagnostic{"Cannot call method requiring mutable 'self' on a temporary value",
                         sema::error::ILLEGAL_RVALUE_CAPTURE,
                         std::pair{6UZ, 20UZ}});
}

} // namespace ghoti::tests
