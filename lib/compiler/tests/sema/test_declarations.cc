#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdx/option.hh>

#include "compiler/ast/expression.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/declaration.hh"

#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

using names_t = std::vector<std::string_view>;

// The parameter names hover would show for the first `name` identifier with a known declaration
auto hover_names(const mod::module& module, std::string_view name) -> stdx::option<names_t> {
    for (const auto id : module.ast.nodes_of<ast::identifier_expr>()) {
        if (module.ast.get_as<ast::identifier_expr>(id).name != name) { continue; }
        if (const auto declaration{module.get_identifier_declaration(id)}) {
            return sema::callable_param_names(*declaration);
        }
    }
    return stdx::none;
}

constexpr std::string_view CALLBACKS{R"(
    pub const Callback := fn(code: i32): bool;
    pub const OnKey := Callback;
    pub const notify := fn(message: []u8, level: u8): void {};
)"};

} // namespace

TEST_CASE("callable parameter names follow aliases declared in another module") {
    // A bodyless `fn(...)` type's own parameter symbols never resolve, so check errors only
    auto [ctx, idx]{helpers::resolve(R"(
        import "cb.gh" as cb;
        const Handler := struct { on_event: cb.Callback, on_key: cb.OnKey };
        const apply := fn(handler: cb.Callback, code: i32): bool { return handler(code); };
        const relay := cb.notify;
    )",
                                     {helpers::mock_file{"cb.gh", CALLBACKS, "cb"}})};
    const auto& root{ctx->root_mod};
    REQUIRE_FALSE(root.is_poisoned());

    CHECK(hover_names(root, "on_event") == names_t{"code"});
    CHECK(hover_names(root, "on_key") == names_t{"code"});
    CHECK(hover_names(root, "handler") == names_t{"code"});
    CHECK(hover_names(root, "relay") == names_t{"message", "level"});
    CHECK(hover_names(root, "notify") == names_t{"message", "level"});
}

TEST_CASE("callable parameter names come from literals, `fn` types, and `dyn Fn` aliases") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const add := fn(lhs: i32, rhs: i32): i32 { return lhs + rhs; };
        const Op := dyn Fn(value: i32): i32;
        const apply := fn(op: Op, raw: fn(first: i32, second: i32): i32): i32 {
            return op(raw(1, 2)) + add(3, 4);
        };
    )")};
    const auto& root{ctx->root_mod};

    CHECK(hover_names(root, "add") == names_t{"lhs", "rhs"});
    CHECK(hover_names(root, "op") == names_t{"value"});
    CHECK(hover_names(root, "raw") == names_t{"first", "second"});
    CHECK_FALSE(hover_names(root, "lhs").has_value());
}

TEST_CASE("a self-referential alias chain stops instead of recursing forever") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const main := fn(): i32 {
            const @"i32": i32 = 42;
            return @"i32";
        };
    )")};
    CHECK_FALSE(hover_names(ctx->root_mod, "i32").has_value());
}

} // namespace ghoti::tests
