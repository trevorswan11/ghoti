#include <algorithm>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace syms = sema::symbols;

namespace {

// Resolves `src` and returns every sema error code the module raised (empty if it resolved clean).
[[nodiscard]] auto resolver_error_codes(std::string_view src) -> std::vector<sema::error> {
    auto [ctx, idx]{helpers::resolve(src)};
    std::vector<sema::error> codes;
    if (const auto diags{ctx->root_mod.diagnostics.as_opt<sema::diagnostics>()}) {
        for (const auto& d : *diags) { codes.push_back(d.get_error()); }
    }
    return codes;
}

[[nodiscard]] auto raised(std::string_view src, sema::error code) -> bool {
    const auto codes{resolver_error_codes(src)};
    return std::ranges::find(codes, code) != codes.end();
}

[[nodiscard]] auto
raised_with_import(std::string_view src, std::string_view other_source, sema::error code) -> bool {
    auto [ctx, idx]{helpers::resolve(src,
                                     helpers::make_vector<helpers::mock_file>(helpers::mock_file{
                                         .path   = "other.gh",
                                         .source = other_source,
                                     }))};

    std::vector<sema::error> codes;
    if (const auto diags{ctx->root_mod.diagnostics.as_opt<sema::diagnostics>()}) {
        for (const auto& d : *diags) { codes.emplace_back(d.get_error()); }
    }
    return std::ranges::find(codes, code) != codes.end();
}

} // namespace

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

TEST_CASE("a conforming trait impl passes and its method is callable") {
    helpers::resolve_and_check(R"(
const W := interface {
    pub const write := fn(&mut self, b: []u8): usize;
    pub const flush := fn(&mut self): void;
};
const File := struct { fd: i32 };
impl W for File {
    pub const write := fn(&mut self, b: []u8): usize { return b.len; };
    pub const flush := fn(&mut self): void {};
}
const use := fn(f: &mut File): usize {
    f.flush();
    return f.write("hi");
};
)");
}

TEST_CASE("a missing requirement is reported") {
    CHECK(raised(R"(
const W := interface { pub const write := fn(&mut self): void; pub const flush := fn(&mut self): void; };
const F := struct { x: i32 };
impl W for F { pub const write := fn(&mut self): void {}; }
)",
                 sema::error::MISSING_IMPL_METHOD));
}

TEST_CASE("a wrong self binding is reported") {
    CHECK(raised(R"(
const W := interface { pub const write := fn(&mut self): void; };
const F := struct { x: i32 };
impl W for F { pub const write := fn(&self): void {}; }
)",
                 sema::error::IMPL_SELF_MISMATCH));
}

TEST_CASE("a wrong return type is reported") {
    CHECK(raised(R"(
const W := interface { pub const size := fn(&self): usize; };
const F := struct { x: i32 };
impl W for F { pub const size := fn(&self): bool { return true; }; }
)",
                 sema::error::IMPL_SIGNATURE_MISMATCH));
}

TEST_CASE("a stray member in a trait impl is reported") {
    CHECK(raised(R"(
const W := interface { pub const write := fn(&mut self): void; };
const F := struct { x: i32 };
impl W for F {
    pub const write := fn(&mut self): void {};
    pub const extra := fn(&self): void {};
}
)",
                 sema::error::UNKNOWN_IMPL_MEMBER));
}

TEST_CASE("the orphan rule rejects an inherent impl on a foreign type") {
    CHECK(raised_with_import(R"(import "other.gh" as other;
impl other::Foreign { pub const f := fn(&self): void {}; }
)",
                             "pub const Foreign := struct { x: i32 };",
                             sema::error::ORPHAN_IMPL));
}

TEST_CASE("two trait impls for the same (I, T) pair are a duplicate") {
    CHECK(raised(R"(
const W := interface { pub const f := fn(&self): void; };
const F := struct { x: i32 };
impl W for F { pub const f := fn(&self): void {}; }
impl W for F { pub const f := fn(&self): void {}; }
)",
                 sema::error::DUPLICATE_IMPL));
}

TEST_CASE("@implements evaluates to the right constexpr bool") {
    helpers::resolve_and_check(R"(
const W := interface { pub const f := fn(&self): void; };
const Yes := struct { x: i32 };
const No := struct { y: i32 };
impl W for Yes { pub const f := fn(&self): void {}; }

const check := fn(): void {
    if constexpr (!@implements(Yes, W)) { @compileError("Yes should implement W"); }
    if constexpr (@implements(No, W)) { @compileError("No must not implement W"); }
    if constexpr (@implements(i32, W)) { @compileError("i32 must not implement W"); }
};
)");
}

TEST_CASE("@implements also accepts a value as its first argument") {
    helpers::resolve_and_check(R"(
const W := interface { pub const f := fn(&self): void; };
const Yes := struct { x: i32 };
impl W for Yes { pub const f := fn(&self): void {}; }
const check := fn(y: Yes): void {
    if constexpr (!@implements(y, W)) { @compileError("value form should agree"); }
};
)");
}

TEST_CASE("an interface cannot be stored by value") {
    CHECK(raised(R"(
const W := interface { pub const f := fn(&self): void; };
var w: W = undefined;
)",
                 sema::error::INTERFACE_NOT_A_VALUE));
}

TEST_CASE("an inherent impl method is callable on an instance") {
    helpers::resolve_and_check(R"(
const P := struct { x: i32, y: i32 };
impl P {
    pub const sum := fn(&self): i32 { return self.x + self.y; };
}
const go := fn(p: &P): i32 { return p.sum(); };
)");
}

} // namespace ghoti::tests
