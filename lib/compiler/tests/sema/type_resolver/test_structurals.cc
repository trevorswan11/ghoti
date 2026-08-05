#include <concepts>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/types.hh>

#include "compiler/ast/statement.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

using helpers::mock_file;
namespace syms = sema::symbols;

namespace {

constexpr std::string_view other_gh{R"(
pub const foo := fn(c: u8): []u8 {};

pub const BarE := enum {
    A,
    pub const bar := fn(&self, c: u8): []u8 {};
};

pub const BarU := union {
    A: i32,
    pub const bar := fn(&self, c: u8): []u8 {};
};

pub const BarS := struct {
    A: i32,
    pub var baz: i32 = 23;
    pub const bar := fn(&self, c: u8): []u8 {};
};
)"};

[[nodiscard]] auto setup_access_test(std::string_view input) -> helpers::ctx_idx_pair {
    return helpers::resolve_and_check(
        fmt::format(R"(import "other.gh" as other; {})", input),
        helpers::make_vector<mock_file>(mock_file{.path = "other.gh", .source = other_gh}));
}

[[nodiscard]] auto u8_slice_type(helpers::sema_test_context& ctx) -> sema::type& {
    return ctx.get_type(sema::type_kind::SLICE, false, ctx.get_type(sema::type_kind::U8));
}

[[nodiscard]] auto bar_fn_type(helpers::sema_test_context& ctx, usize table_idx) -> sema::type& {
    return ctx.get_type(sema::type_kind::FUNCTION, table_idx);
}

auto check_access_decl(helpers::sema_test_context& ctx,
                       usize                       idx,
                       std::string_view            symbol_name,
                       const sema::type&           expected_type) -> void {
    const auto [sym, data, type]{ctx.get_type_sym_info<syms::node_t>(symbol_name, idx)};
    CHECK(expected_type == type);
}

template <std::same_as<sema::diagnostic>... Ds>
auto test_access_fail(std::string_view input, Ds&&... diagnostics) -> void {
    helpers::test_resolver_fail(
        fmt::format(R"(import "other.gh" as other; {})", input),
        helpers::make_vector<mock_file>(mock_file{.path = "other.gh", .source = other_gh}),
        std::forward<Ds>(diagnostics)...);
}

} // namespace

TEST_CASE("Free function resolved access") {
    auto [ctx, idx]{setup_access_test("const a := other::foo('a');")};
    check_access_decl(*ctx, idx, "a", u8_slice_type(*ctx));
}

TEST_CASE("Enum resolved access") {
    auto [ctx, idx]{setup_access_test(R"(
var e1: other::BarE = .A;
const e2 := other::BarE.A;
const e3 := e1.bar('a');

const func := other::BarE.bar;
)")};

    const auto& enum_type{ctx->get_type(sema::type_kind::ENUM, 3)};
    check_access_decl(*ctx, idx, "e1", enum_type);
    check_access_decl(*ctx, idx, "e2", enum_type);
    check_access_decl(*ctx, idx, "e3", u8_slice_type(*ctx));
    check_access_decl(*ctx, idx, "func", bar_fn_type(*ctx, 4));
}

TEST_CASE("Union resolved access") {
    auto [ctx, idx]{setup_access_test(R"(
var u1: other::BarU = .{ .A = 1, };
const u2 := other::BarU{ .A = 1, };
const u3 := u1.bar('a');

const func := other::BarU.bar;
)")};

    const auto& union_type{ctx->get_type(sema::type_kind::UNION, 5)};
    check_access_decl(*ctx, idx, "u1", union_type);
    check_access_decl(*ctx, idx, "u2", union_type);
    check_access_decl(*ctx, idx, "u3", u8_slice_type(*ctx));
    check_access_decl(*ctx, idx, "func", bar_fn_type(*ctx, 6));
}

TEST_CASE("Struct resolved access") {
    auto [ctx, idx]{setup_access_test(R"(
var s1: other::BarS = .{ .A = 42, };
const s2 := other::BarS{ .A = 1, };
const s3 := s1.baz;
const s4 := s1.bar('a');

const member := other::BarS.baz;
const func := other::BarS.bar;
)")};

    const auto& struct_type{ctx->get_type(sema::type_kind::STRUCT, 7)};
    const auto& member_type{ctx->get_type(sema::type_kind::I32)};

    check_access_decl(*ctx, idx, "s1", struct_type);
    check_access_decl(*ctx, idx, "s2", struct_type);
    check_access_decl(*ctx, idx, "s3", member_type);
    check_access_decl(*ctx, idx, "s4", u8_slice_type(*ctx));
    check_access_decl(*ctx, idx, "member", member_type);
    check_access_decl(*ctx, idx, "func", bar_fn_type(*ctx, 8));
}

TEST_CASE("Indirection in structural type resolution") {
    helpers::resolve_and_check("const A := struct { a: ^B, }; const B := struct { b: ^A, };");
    helpers::resolve_and_check("const A := struct { a: ^A, };");
    helpers::resolve_and_check("const A := struct { a: ^A, const b := fn(c: A): i32 {}; };");
    helpers::resolve_and_check("const A := struct { a: ^@this(), const b := fn(c: ^A): i32 {}; };");
    helpers::resolve_and_check(
        R"(const A := struct {
            a: ^A,
            const b := fn(&self, c: A): i32 {
                const dA := A;
                using uA = A;
            };
        };)");
}

TEST_CASE("Legal circular module-based access resolution") {
    constexpr std::string_view a_gh{R"(import "b.gh" as b; const A := struct { field: i32, };)"};
    constexpr std::string_view b_gh{R"(import "a.gh" as a; const B := struct { field: i32, };)"};

    helpers::resolve_and_check(
        R"(import "a.gh" as a;)",
        helpers::make_vector<mock_file>(mock_file{.path = "a.gh", .source = a_gh},
                                        mock_file{.path = "b.gh", .source = b_gh}));
}

TEST_CASE("Access through non-user-type types") {
    test_access_fail(
        "var a: i32 = .z;",
        sema::diagnostic{
            "Can only access inner objects inside of structs, unions, and enums; found 'i32'",
            sema::error::TYPE_MISMATCH,
            std::pair{0UZ, 41UZ}});
}

TEST_CASE("Implicitly accessing unknown user-type fields/members") {
    const auto expected_diag = [] -> sema::diagnostic {
        return {"Type has no field named 'z'",
                sema::error::UNDECLARED_IDENTIFIER,
                std::pair{0UZ, 50UZ}};
    };

    test_access_fail("var a: other::BarE = .z;", expected_diag());
    test_access_fail("var a: other::BarU = .z;", expected_diag());
    test_access_fail("var a: other::BarS = .z;", expected_diag());
}

TEST_CASE("Dot-accessing unknown user-type fields/members") {
    const auto expected_diag = [](std::string_view type_name) -> sema::diagnostic {
        return {fmt::format("Type '{}' has no field named 'z'", type_name),
                sema::error::UNDECLARED_IDENTIFIER,
                std::pair{0UZ, 49UZ}};
    };

    test_access_fail("var a := other::BarE.z;", expected_diag("BarE"));
    test_access_fail("var a := other::BarU.z;", expected_diag("BarU"));
    test_access_fail("var a := other::BarS.z;", expected_diag("BarS"));
}

TEST_CASE("Illegal module access targets") {
    const auto expected_diag = [](std::string_view type_name) -> sema::diagnostic {
        return {
            fmt::format("Use the dot operator '.' to access {} fields; found module access '::'",
                        type_name),
            sema::error::TYPE_MISMATCH,
            std::pair{0UZ, 42UZ}};
    };

    test_access_fail("var a := other::BarE::e;", expected_diag("enum"));
    test_access_fail("var a := other::BarU::e;", expected_diag("union"));
    test_access_fail("var a := other::BarS::e;", expected_diag("struct"));

    helpers::test_resolver_fail(
        "var a := i32::a;",
        sema::diagnostic{"Module access operator '::' can only be applied to modules; found 'i32'",
                         sema::error::TYPE_MISMATCH,
                         std::pair{0UZ, 9UZ}});
}

TEST_CASE("Unknown member lookup in module") {
    test_access_fail("var a := other::BarF;",
                     sema::diagnostic{"Module 'other' has no member named 'BarF'",
                                      sema::error::UNDECLARED_IDENTIFIER,
                                      std::pair{0UZ, 44UZ}});
}

TEST_CASE("Incomplete type used during resolution") {
    const auto expected_diag = [](usize col) -> sema::diagnostic {
        return {"Field 'a' has an incomplete type; creates an infinite size cycle",
                sema::error::CYCLIC_DEPENDENCY,
                std::pair{0UZ, col}};
    };

    SECTION("Structs") {
        helpers::test_resolver_fail("const A := struct { a: A, };", expected_diag(23));
        helpers::test_resolver_fail("const A := struct { a: @this(), };", expected_diag(23));
        helpers::test_resolver_fail("const A := struct { a: B, }; const B := struct { b: A, };",
                                    expected_diag(23));
    }

    SECTION("Unions") {
        helpers::test_resolver_fail("const A := union { a: A, };", expected_diag(22));
        helpers::test_resolver_fail("const A := union { a: @this(), };", expected_diag(22));
        helpers::test_resolver_fail("const A := union { a: B, }; const B := union { b: A, };",
                                    expected_diag(22));
    }
}

TEST_CASE("Illegal circular module-based access resolution") {
    constexpr std::string_view a_gh{
        R"(import "b.gh" as b; pub const A := struct { field: b::B, };)"};
    constexpr std::string_view b_gh{
        R"(import "a.gh" as a; pub const B := struct { field: a::A, };)"};

    auto [ctx, idx]{helpers::resolve(
        R"(import "a.gh" as a; using A = a::A;)",
        helpers::make_vector<mock_file>(mock_file{.path = "a.gh", .source = a_gh},
                                        mock_file{.path = "b.gh", .source = b_gh}))};
    auto& test_module{*UNWRAP(ctx->manager.try_get_file_module("test.gh"))};
    auto& a_module{*UNWRAP(ctx->manager.try_get_file_module("a.gh"))};
    auto& b_module{*UNWRAP(ctx->manager.try_get_file_module("b.gh"))};

    helpers::check_errors_against<sema::diagnostics>(
        b_module,
        sema::diagnostic{"Cross-module cyclic dependency detected while resolving symbol 'B'",
                         sema::error::CYCLIC_DEPENDENCY,
                         std::pair{0UZ, 54UZ}});

    ctx->check_poisoned<syms::node_t>("A", idx, test_module);
    ctx->check_poisoned<syms::node_t>("A", 1, a_module);
    ctx->check_poisoned<syms::node_t>("B", 2, b_module);

    // Errors dont get bubbled up to simplify handling, so only one module is poisoned
    const auto [sym, _]{ctx->get_symbol<syms::node_t>("b", 1)};
    ctx->check_poisoned(sym);
}

TEST_CASE("Initializer expression in various resolution contexts") {
    SECTION("Out of place access-related expressions") {
        helpers::test_resolver_fail(
            ".{};",
            sema::diagnostic{
                "Initializer expression requires a known type; provide an explicit type "
                "or use in a typed context",
                sema::error::TYPE_MISMATCH,
                std::pair{0UZ, 1UZ}});

        helpers::test_resolver_fail(
            "const a := .f;",
            sema::diagnostic{"Implicit access expression used outside of a typed context",
                             sema::error::TYPE_MISMATCH,
                             std::pair{0UZ, 11UZ}});
    }

    SECTION("Initializer with incomplete type") {
        const auto expected_diag = [](usize col) -> sema::diagnostic {
            return {"Cannot initialize an incomplete type",
                    sema::error::CYCLIC_DEPENDENCY,
                    std::pair{0UZ, col}};
        };

        helpers::test_resolver_fail("struct { a: auto = @this(){}, };", expected_diag(26));
        helpers::test_resolver_fail("struct { a: @this() = .{}, };", expected_diag(23));
    }

    SECTION("Union & enum type restrictions") {
        helpers::test_resolver_fail(
            "const U := union { A: i32, B: i32, }; const u := U{ .A = 1, .B = 2 };",
            sema::diagnostic{"Union initializer lists must list exactly one field; found 2",
                             sema::error::ARITY_MISMATCH,
                             std::pair{0UZ, 50UZ}});

        helpers::test_resolver_fail(
            "const E := enum { A, }; const e := E{};",
            sema::diagnostic{"Enums cannot be initialized with an initializer expression as they "
                             "lack member variables",
                             sema::error::ARITY_MISMATCH,
                             std::pair{0UZ, 36UZ}});
    }

    SECTION("Struct type restrictions") {
        constexpr std::string_view input{"const S := struct { a: i32, b: i64 = 3, c: bool,};"};
        helpers::resolve_and_check(
            fmt::format("{} const a: S = .{{.a = 2, .b = 3, .c = false, }};", input));

        helpers::test_resolver_fail(
            fmt::format("{} const a: S = .{{.a = 2, .b = 3, .c = false, .c = true, }};", input),
            sema::diagnostic{"Struct initializer contains duplicate field: c",
                             sema::error::DUPLICATE_FIELD,
                             std::pair{0UZ, 65UZ}});
        helpers::test_resolver_fail(
            fmt::format("{} const a: S = .{{.a = 2, .b = 3, .c = false, .c = true, .a = 34 }};",
                        input),
            sema::diagnostic{"Struct initializer contains duplicate fields: c, a",
                             sema::error::DUPLICATE_FIELD,
                             std::pair{0UZ, 65UZ}});

        helpers::test_resolver_fail(fmt::format("{} const a: S = .{{ .a = 2, }};", input),
                                    sema::diagnostic{"Struct initializer missing required field: c",
                                                     sema::error::MISSING_FIELD,
                                                     std::pair{0UZ, 65UZ}});
        helpers::test_resolver_fail(
            fmt::format("{} const a: S = .{{ .b = 2, }};", input),
            sema::diagnostic{"Struct initializer missing required fields: a, c",
                             sema::error::MISSING_FIELD,
                             std::pair{0UZ, 65UZ}});

        helpers::test_resolver_fail(
            fmt::format("{} const a: S = .{{ .a = 2, .c = true, .d = 2 }};", input),
            sema::diagnostic{"Struct initializer contains unknown field: d",
                             sema::error::UNKNOWN_FIELD,
                             std::pair{0UZ, 65UZ}});
    }

    SECTION("Slice field access") {
        const auto [ctx, idx]{helpers::resolve_and_check(R"(
            const s: []u8 = @sliceFromPtr(^1, 10UZ);
            const p := s.ptr;
            const l := s.len;
        )")};
        check_access_decl(
            *ctx,
            idx,
            "p",
            ctx->get_type(sema::type_kind::POINTER, ctx->get_type(sema::type_kind::U8)));
        check_access_decl(*ctx, idx, "l", ctx->get_type(sema::type_kind::USIZE));
    }
}

TEST_CASE("Struct field access through dereferenced pointer and reference") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Point := struct { x: i32, y: i32 };
        const pt_ptr: ^Point = undefined;
        const pt_ref: &Point = undefined;

        const px := pt_ptr.x;
        const ry := pt_ref.y;
    )")};

    const auto [px_sym, _, px_decl, px_type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("px", idx)};
    CHECK(px_type.get_kind() == sema::type_kind::I32);

    const auto [ry_sym, _r, ry_decl, ry_type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("ry", idx)};
    CHECK(ry_type.get_kind() == sema::type_kind::I32);
}

} // namespace ghoti::tests
