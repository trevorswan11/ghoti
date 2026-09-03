#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace syms = sema::symbols;

TEST_CASE("interface resolves to an interface_t carrying its members") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
const W := interface {
    Error: type;
    Item: type = u8;
    const cap: usize = 4096;
    pub const write := fn(&mut self, b: []u8): usize;
    const dbg := fn(&self): []u8;
    pub const writeAll := fn(&mut self, b: []u8): usize { return self.write(b); };
};
)")};

    const auto [sym, data, type]{ctx->get_type_sym_info<syms::node_t>("W", idx)};
    REQUIRE(type.get_kind() == sema::type_kind::INTERFACE);
    CHECK_FALSE(sema::is_value_type(type.get_kind()));

    const auto iface{type.get_data().as_opt<sema::types::interface_t>()};
    REQUIRE(iface.has_value());
    CHECK(iface->ast_methods.size() == 3);
    CHECK(iface->ast_assoc_types.size() == 2);
    CHECK(iface->ast_assoc_consts.size() == 1);
}

TEST_CASE("an empty marker interface resolves") {
    auto [ctx, idx]{helpers::resolve_and_check("const Marker := interface {};")};
    const auto [sym, data, type]{ctx->get_type_sym_info<syms::node_t>("Marker", idx)};
    REQUIRE(type.get_kind() == sema::type_kind::INTERFACE);
    const auto iface{type.get_data().as_opt<sema::types::interface_t>()};
    REQUIRE(iface.has_value());
    CHECK(iface->ast_methods.empty());
}

TEST_CASE("an interface flows through a const binding as a type value") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
const W := interface { pub const f := fn(&self): void; };
const Alias := W;
)")};
    const auto [sym, data, type]{ctx->get_type_sym_info<syms::node_t>("Alias", idx)};
    CHECK(type.get_kind() == sema::type_kind::INTERFACE);
}

TEST_CASE("an inherent impl block resolves in phase 1") {
    helpers::resolve_and_check(R"(
const S := struct { x: i32 };

impl S {
    pub const fromRaw := fn(v: i32): @this() { return .{ .x = v }; };
}
)");
}

TEST_CASE("a trait impl block resolves in phase 1") {
    helpers::resolve_and_check(R"(
const S := struct { x: i32 };
const W := interface { pub const write := fn(&mut self): void; };

impl W for S {
    pub const write := fn(&mut self): void {};
}
)");
}

TEST_CASE("a parameterized impl is accepted but left un-resolved in phase 1") {
    helpers::resolve_and_check(R"(
const S := struct { x: i32 };

impl(P: type) S {
    pub const tag := fn(): void {};
}
)");
}

} // namespace ghoti::tests
