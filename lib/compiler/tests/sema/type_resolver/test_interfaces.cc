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
    CHECK(helpers::raised(R"(
        const W := interface { pub const write := fn(&mut self): void; pub const flush := fn(&mut self): void; };
        const F := struct { x: i32 };
        impl W for F { pub const write := fn(&mut self): void {}; }
)",
                          sema::error::MISSING_IMPL_METHOD));
}

TEST_CASE("a wrong self binding is reported") {
    CHECK(helpers::raised(R"(
        const W := interface { pub const write := fn(&mut self): void; };
        const F := struct { x: i32 };
        impl W for F { pub const write := fn(&self): void {}; }
)",
                          sema::error::IMPL_SELF_MISMATCH));
}

TEST_CASE("a wrong return type is reported") {
    CHECK(helpers::raised(R"(
        const W := interface { pub const size := fn(&self): usize; };
        const F := struct { x: i32 };
        impl W for F { pub const size := fn(&self): bool { return true; }; }
)",
                          sema::error::IMPL_SIGNATURE_MISMATCH));
}

TEST_CASE("a stray member in a trait impl is reported") {
    CHECK(helpers::raised(R"(
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
    CHECK(helpers::raised(R"(
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
    CHECK(helpers::raised(R"(
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

TEST_CASE("an inherent impl member may not shadow a native member or another impl member") {
    CHECK(helpers::raised(R"(
        const P := struct { x: i32, const m := fn(&self): i32 { return x; }; };
        impl P { pub const m := fn(&self): i32 { return 0; }; }
)",
                          sema::error::DUPLICATE_MEMBER));

    CHECK(helpers::raised(R"(
        const P := struct { x: i32 };
        impl P { pub const m := fn(&self): i32 { return 1; }; }
        impl P { pub const m := fn(&self): i32 { return 2; }; }
)",
                          sema::error::DUPLICATE_MEMBER));
}

TEST_CASE("a default method resolves on the implementing type") {
    helpers::resolve_and_check(R"(
        const It := interface {
            pub const next := fn(&mut self): i32;
            pub const twice := fn(&mut self): i32 { return self.next() + self.next(); };
        };
        const Src := struct { v: i32 };
        impl It for Src { pub const next := fn(&mut self): i32 { return self.v; }; }
        const run := fn(s: &mut Src): i32 { return s.twice(); };
)");
}

TEST_CASE("an `impl I` parameter accepts a conforming argument and rejects a non-conforming one") {
    helpers::resolve_and_check(R"(
        const W := interface { pub const write := fn(&self, n: i32): i32; };
        const File := struct { fd: i32 };
        impl W for File { pub const write := fn(&self, n: i32): i32 { return self.fd + n; }; }
        const dump := fn(w: &impl W, n: i32): i32 { return w.write(n); };
        const go := fn(f: &File): i32 { return dump(f, 3); };
)");

    CHECK(helpers::raised(R"(
        const W := interface { pub const write := fn(&self): void; };
        const Nope := struct { x: i32 };
        const dump := fn(w: &impl W): void { w.write(); };
        const go := fn(n: &Nope): void { dump(n); };
)",
                          sema::error::UNSATISFIED_BOUND));
}

TEST_CASE("a static `var` is allowed inside a trait impl") {
    helpers::resolve_and_check(R"(
        const W := interface { pub const write := fn(&mut self): void; };
        const F := struct { x: i32 };
        impl W for F {
            var calls: i32 = 0;
            pub const write := fn(&mut self): void {};
        }
)");
}

TEST_CASE("an `impl (A + B)` bound with a shared associated item is rejected") {
    CHECK(helpers::raised(R"(
        const A := interface { Item: type; pub const a := fn(&self): void; };
        const B := interface { Item: type; pub const b := fn(&self): void; };
        const use := fn(x: &impl (A + B)): void { x.a(); };
)",
                          sema::error::CONFLICTING_ASSOC));
}

TEST_CASE("an `impl (A + B)` parameter requires both interfaces") {
    helpers::resolve_and_check(R"(
        const R := interface { pub const rd := fn(&self): i32; };
        const W := interface { pub const wr := fn(&self): i32; };
        const Dev := struct { a: i32, b: i32 };
        impl R for Dev { pub const rd := fn(&self): i32 { return self.a; }; }
        impl W for Dev { pub const wr := fn(&self): i32 { return self.b; }; }
        const tee := fn(x: &impl (R + W)): i32 { return x.rd() + x.wr(); };
        const go := fn(d: &Dev): i32 { return tee(d); };
)");

    CHECK(helpers::raised(R"(
        const R := interface { pub const rd := fn(&self): i32; };
        const W := interface { pub const wr := fn(&self): i32; };
        const HalfDev := struct { a: i32 };
        impl R for HalfDev { pub const rd := fn(&self): i32 { return self.a; }; }
        const tee := fn(x: &impl (R + W)): i32 { return x.rd(); };
        const go := fn(d: &HalfDev): i32 { return tee(d); };
)",
                          sema::error::UNSATISFIED_BOUND));
}

TEST_CASE("a parameterized inherent impl expands for each concrete instantiation") {
    helpers::resolve_and_check(R"(
        const Box := fn(T: type): type { return struct { v: T }; };
        impl(T: type) Box(T) {
            pub const get := fn(&self): T { return self.v; };
        }
        const use := fn(): i32 {
            var b: Box(i32) = .{ .v = 7 };
            return b.get();
        };
)");
}

TEST_CASE("a parameterized trait impl over a local ctor conforms and its method is callable") {
    helpers::resolve_and_check(R"(
        const Show := interface { pub const show := fn(&self): i32; };
        const Box := fn(T: type): type { return struct { v: T }; };
        impl(T: type) Show for Box(T) {
            pub const show := fn(&self): i32 { return self.v; };
        }
        const use := fn(): i32 {
            var b: Box(i32) = .{ .v = 5 };
            return b.show();
        };
)");
}

TEST_CASE("a parameterized impl anchored on neither its ctor nor its interface is an orphan") {
    CHECK(raised_with_import(R"(
        import "other.gh" as other;
        impl(T: type) other::Bag(T) { pub const peek := fn(&self): T { return self.v; }; }
)",
                             R"(pub const Bag := fn(T: type): type { return struct { v: T }; };)",
                             sema::error::ORPHAN_IMPL));
}

TEST_CASE("a parameterized impl with several type params conforms per instantiation") {
    helpers::resolve_and_check(R"(
        const Both := interface {
            pub const lhs := fn(&self): i32;
            pub const rhs := fn(&self): i32;
        };
        const Pair := fn(A: type, B: type): type { return struct { a: A, b: B }; };
        impl(A: type, B: type) Both for Pair(A, B) {
            pub const lhs := fn(&self): i32 { return self.a; };
            pub const rhs := fn(&self): i32 { return self.b; };
        }
        const use := fn(): i32 {
            var p: Pair(i32, i32) = .{ .a = 1, .b = 2 };
            return p.lhs() + p.rhs();
        };
)");
}

TEST_CASE("a `constexpr` parameterized-impl param resolves as a value and an array dimension") {
    helpers::resolve_and_check(R"(
        const Buf := fn(constexpr cap: usize): type { return struct { head: i32 }; };
        impl(constexpr n: usize) Buf(n) {
            pub const cap := fn(&self): usize { return n; };
            pub const scratch := fn(&self): i32 {
                var tmp: [n]i32 = undefined;
                const first: i32 = tmp[0];
                return first - first + self.head + @as(i32, n / 2);
            };
        }
        const use := fn(): i32 {
            var b: Buf(8) = .{ .head = 1 };
            return @as(i32, b.cap()) + b.scratch();
        };
)");
}

TEST_CASE("a parameterized impl anchored on a local interface may target a foreign ctor") {
    auto [ctx, idx]{helpers::resolve(
        R"(
        import "other.gh" as other;
        const Named := interface { pub const label := fn(&self): i32; };
        impl(T: type) Named for other::Bag(T) {
            pub const label := fn(&self): i32 { return self.v; };
        }
        const use := fn(): i32 {
            var b: other::Bag(i32) = .{ .v = 7 };
            return b.label();
        };
)",
        helpers::make_vector<helpers::mock_file>(helpers::mock_file{
            .path   = "other.gh",
            .source = "pub const Bag := fn(T: type): type { return struct { v: i32 }; };",
        }))};
    std::vector<sema::error> codes;
    if (const auto diags{ctx->root_mod.diagnostics.as_opt<sema::diagnostics>()}) {
        for (const auto& d : *diags) { codes.emplace_back(d.get_error()); }
    }
    CHECK(codes.empty());
}

} // namespace ghoti::tests
