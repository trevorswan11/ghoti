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
    const auto check_bi_type = [](std::string_view value, sema::type_kind expected_kind) -> void {
        auto [ctx, idx]{helpers::resolve_and_check(fmt::format("const a := {};", value))};
        const auto [sym, data, type]{ctx->get_type_sym_info<syms::node_t>("a", idx)};
        CHECK(type == ctx->get_type(expected_kind));
    };

    using tk = sema::type_kind;
    check_bi_type("1", tk::I32);
    check_bi_type("1l", tk::I64);
    check_bi_type("1z", tk::ISIZE);
    check_bi_type("1u", tk::U32);
    check_bi_type("1ul", tk::U64);
    check_bi_type("1UZ", tk::USIZE);
    check_bi_type("'1'", tk::U8);
    check_bi_type("true", tk::BOOL);
    check_bi_type("{}", tk::VOID_);
    check_bi_type("undefined", tk::UNDEFINED);
    check_bi_type("unreachable", tk::NORETURN);
    check_bi_type("1.0f", tk::F32);
    check_bi_type("1.0", tk::F64);
}

TEST_CASE("Nested type resolution") {
    auto [ctx, idx]{helpers::resolve_and_check("var a: ^^i32; var b: ^^^i32;")};

    const auto& i32_ptr =
        ctx->get_type(sema::type_kind::POINTER, ctx->get_type(sema::type_kind::I32));
    const auto& i32_ptr_ptr{ctx->get_type(sema::type_kind::POINTER, i32_ptr)};

    const auto [a_sym, a_sym_data, a_type]{ctx->get_type_sym_info<syms::node_t>("a", idx)};
    CHECK(a_type == i32_ptr_ptr);
    const auto [b_sym, b_sym_data, b_type]{ctx->get_type_sym_info<syms::node_t>("b", idx)};
    CHECK(b_type == ctx->get_type(sema::type_kind::POINTER, i32_ptr_ptr));
}

TEST_CASE("Type alias resolution") {
    auto [ctx, idx]{helpers::resolve_and_check("using a = ^bool; var b: a; var c: &a;")};
    const auto& bool_ref =
        ctx->get_type(sema::type_kind::POINTER, ctx->get_type(sema::type_kind::BOOL));

    const auto [a_sym, a_sym_data, a_type]{ctx->get_type_sym_info<syms::node_t>("a", idx)};
    CHECK(a_type == bool_ref);
    const auto [b_sym, b_sym_data, b_type]{ctx->get_type_sym_info<syms::node_t>("b", idx)};
    CHECK(b_type == bool_ref);
    const auto [c_sym, c_sym_data, c_type]{ctx->get_type_sym_info<syms::node_t>("c", idx)};
    CHECK(c_type == ctx->get_type(sema::type_kind::REFERENCE, bool_ref));
}

TEST_CASE("Unary expression resolution") {
    helpers::resolve_and_check("var a: ^i32; _ = *a;");
    helpers::resolve_and_check("_ = !1;");
    helpers::resolve_and_check("_ = ~1;");
    helpers::resolve_and_check("_ = -1;");

    helpers::test_resolver_fail(
        "var a: i32; _ = *a;",
        sema::diagnostic{"Cannot dereference non-pointer expression; found 'i32'",
                         sema::error::TYPE_MISMATCH,
                         std::pair{0UZ, 16UZ}});
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
    helpers::test_resolver_fail("using a = ^b;", expected_diag(10));
}

TEST_CASE("Defer & discard statement resolution") {
    auto [ctx, idx]{helpers::resolve_and_check("fn(): void { defer { var a: i32; } }")};
    const auto [sym, _, type]{ctx->get_type_sym_info<syms::node_t>("a", 2)};
    CHECK(type == ctx->get_type(sema::type_kind::I32));
    helpers::resolve_and_check("_ = 1 + 1;");
}

TEST_CASE("Call resolution edge cases") {
    helpers::resolve_and_check(
        "@sizeOf(blk: { if (1 + 1 == 2) { break :blk i32; } else { break :blk f64; } });");
    helpers::resolve_and_check("@typeOf([]i32);");
    helpers::test_resolver_fail("const a := b; const c := a();",
                                sema::diagnostic{"Use of undeclared identifier 'b'",
                                                 sema::error::UNDECLARED_IDENTIFIER,
                                                 std::pair{0UZ, 11UZ}});
}

TEST_CASE("Loop resolution") {
    helpers::resolve_and_check("const a := loop { const foo := 42; };");
    helpers::test_resolver_fail(
        "for (23) |_| { var a: i32; }",
        sema::diagnostic{"Iterables may only be arrays or slices; found 'i32'",
                         sema::error::TYPE_MISMATCH,
                         std::pair{0UZ, 5UZ}});
}

TEST_CASE("Duplicate test name") {
    helpers::test_resolver_fail(
        R"(test "TEST ME" { var a: i32; } test "TEST ME" { var a: i32; })",
        sema::diagnostic{"Duplicate test block named 'TEST ME'; previous declaration here: 1:1",
                         sema::error::DUPLICATE_TEST_NAME,
                         std::pair{0UZ, 31UZ}});
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

} // namespace ghoti::tests
