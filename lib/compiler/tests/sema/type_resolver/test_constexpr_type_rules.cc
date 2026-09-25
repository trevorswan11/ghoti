#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/types.hh>

#include "compiler/module/module.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("`constexpr` on a `type` parameter is redundant") {
    helpers::test_resolver_fail(
        "const f := fn(constexpr t: type): i32 { _ = t; return 0; };",
        sema::diagnostic{"'constexpr' is redundant on a parameter of type 'type'; type values "
                         "are always compile-time known",
                         sema::error::REDUNDANT_CONSTEXPR,
                         std::pair{0UZ, 24UZ}});
}

TEST_CASE("`constexpr` on a parameter typed by an earlier generic type param isn't redundant") {
    helpers::resolve_and_check(
        "const Point := fn(T: type, constexpr default_z: T): type { return T; };");
}

TEST_CASE("a `var` binding cannot hold a `type` value") {
    const auto mutable_type_diag = [](usize col) {
        return sema::diagnostic{"a 'type' value cannot be stored in a mutable ('var') binding; "
                                "use 'const' or 'constexpr' instead",
                                sema::error::MUTABLE_TYPE_BINDING,
                                std::pair{0UZ, col}};
    };

    helpers::test_resolver_fail("var a: type = i32;", mutable_type_diag(0UZ));
    helpers::test_resolver_fail("var a := i32;", mutable_type_diag(0UZ));
    helpers::test_resolver_fail("var a := ^mut i32;", mutable_type_diag(0UZ));
    helpers::test_resolver_fail("var a := []u8;", mutable_type_diag(0UZ));
    helpers::test_resolver_fail("const S := struct { x: i32 }; var a := S;",
                                mutable_type_diag(30UZ));
    helpers::test_resolver_fail(
        "const Box := fn(T: type): type { return struct { v: T }; }; var a := Box(i32);",
        mutable_type_diag(60UZ));

    helpers::resolve_and_check("const a: type = i32;");
    helpers::resolve_and_check("const a := i32;");
    helpers::resolve_and_check("constexpr a: type = i32;");
    helpers::resolve_and_check("constexpr a := i32;");
    helpers::resolve_and_check("const S := struct { x: i32 };");
    helpers::resolve_and_check("const f := fn(t: type): i32 { _ = t; return 0; };");

    helpers::resolve_and_check("const P := struct { x: i32 }; var p: P = undefined;");
    helpers::resolve_and_check("var arr: [2uz]i32 = [_]i32{1, 2};");
}

TEST_CASE("a `var` binding cannot hold an aggregate of `type`s") {
    const auto mutable_holder_diag = [](std::string_view type_name, usize col) {
        return sema::diagnostic{
            fmt::format("a value of type '{}' holds 'type's and only exists at compile time, so it "
                        "cannot be stored in a mutable ('var') binding; use 'const' or 'constexpr' "
                        "instead",
                        type_name),
            sema::error::MUTABLE_TYPE_BINDING,
            std::pair{0UZ, col}};
    };

    helpers::test_resolver_fail("var ts := [_]type{ i32, u8 };", mutable_holder_diag("[2]type", 0));
    helpers::test_resolver_fail("var ts: [2]type = undefined;", mutable_holder_diag("[2]type", 0));
    helpers::test_resolver_fail("const S := struct { t: type }; var s := S{ .t = i32 };",
                                mutable_holder_diag("S", 31));

    helpers::resolve_and_check("const ts := [_]type{ i32, u8 };");
    helpers::resolve_and_check("constexpr ts := [_]type{ i32, u8 };");
    helpers::resolve_and_check("constexpr ts: [2]type = .{ i32, u8 };");
    helpers::resolve_and_check("constexpr ts: []type = ^.{ i32, u8 };");
    helpers::resolve_and_check("const S := struct { t: type }; constexpr s := S{ .t = i32 };");
}

TEST_CASE("an aggregate of `type`s is a storageless compile-time value") {
    using storageless_kind = mod::storageless_kind;
    const auto kind_of     = [](helpers::sema_test_context& ctx, usize idx, std::string_view name) {
        const auto [sym, node]{ctx.get_symbol<sema::symbols::node_t>(name, idx)};
        return ctx.root_mod.get_storageless_kind(node);
    };

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const ts := [_]type{ i32, u8 };
        constexpr cts := [_]type{ bool, f32 };
        constexpr nested := [_][2]type{ .{ i32, u8 }, .{ bool, f32 } };
        const S := struct { t: type, n: i32 };
        constexpr s := S{ .t = i32, .n = 1 };
        const T := i32;
        const xs := [_]i32{ 1, 2 };
        const first := ts[0];
        const len := ts.len;
    )")};

    CHECK(kind_of(*ctx, idx, "ts") == storageless_kind::CONSTEXPR_VALUE);
    CHECK(kind_of(*ctx, idx, "cts") == storageless_kind::CONSTEXPR_VALUE);
    CHECK(kind_of(*ctx, idx, "nested") == storageless_kind::CONSTEXPR_VALUE);
    CHECK(kind_of(*ctx, idx, "s") == storageless_kind::CONSTEXPR_VALUE);

    // A struct holding a `type` field is itself still just a type
    CHECK_FALSE(kind_of(*ctx, idx, "S"));
    CHECK(UNWRAP(kind_of(*ctx, idx, "T")) == storageless_kind::ALIAS);
    CHECK(UNWRAP(kind_of(*ctx, idx, "first")) != storageless_kind::CONSTEXPR_VALUE);
    CHECK_FALSE(kind_of(*ctx, idx, "xs"));
    CHECK_FALSE(kind_of(*ctx, idx, "len"));
}

} // namespace ghoti::tests
