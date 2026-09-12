#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

using helpers::mock_file;

// A `var` global's declared type is mutable, but a by-value `self` parameter conventionally
// resolves to the *const* sibling of the same aggregate (`type_pool::with_const`) - two distinct
// `sema::type` objects sharing the same `struct_t` data. `codegen::type_translator` used to cache
// each aggregate's LLVM struct type by that raw `sema::type*`, so the two siblings got separate,
// structurally-identical-but-distinct named LLVM types, tripping LLVM's own "calling a function
// with bad signature" check the moment the `var`'s value crossed into the by-value `self` slot.
TEST_CASE("A `var` global's struct value passes cleanly through a by-value `self` method") {
    CHECK(helpers::compile_and_run(R"(
        const Box := struct {
            v: i32,
            pub const get := fn(self): i32 { return self.v; };
        };
        var g: Box = .{ .v = 9 };
        pub const main := fn(): i32 {
            return g.get();
        };
    )") == 9);
}

// A qualified `mod.member` read of a `var`/aggregate-`const` global used to try folding the
// member's own *initializer* expression first (via `const_eval::try_eval`), rather than reading
// its real storage - wrong for a `var` (whose value can change at runtime) and, even for the
// untouched-since-init case, silently degrading to a bogus `void` value whenever the fold produced
// a shape `to_gir_value()` has no scalar representation for (e.g. a struct holding a `^mut`
// address-of-another-global field, as below). `global_ref_in`'s storage-backed read must run
// first - and, since the member's own `decl_stmt` lives in the *imported* module's AST, needs that
// module swapped in for the lookup rather than assuming "whichever module is currently emitting".
TEST_CASE("A cross-module qualified read of a `var` struct global reads its real storage") {
    constexpr std::string_view HELPER{R"(
        pub const Cell := struct { pub p: ^i32 };
        var backing: i32 = 41;
        pub var cell: Cell = .{ .p = ^backing };
    )"};
    CHECK(helpers::compile_and_run(
              R"(
        import "helper.gh" as helper;
        pub const main := fn(): i32 {
            return *helper.cell.p;
        };
    )",
              {mock_file{"helper.gh", HELPER, "helper"}}) == 41);
}

// `emit_dyn_coercion` registers its synthesised `__vtable.<N>` global on the module currently
// being emitted (`active_mod()`) - wrong when the coercion happens while lazily emitting an
// *imported* module's own function body (e.g. a helper returning a `^dyn I` it built internally):
// `gir::module` (and so `llvm_lowering::lower_dyn_vtables`) only ever lowers the *root* module's
// own `dyn_vtables` list into a real LLVM global, so a vtable registered on any other module's
// list is silently never materialized - the coercion's own vtable-half store gets dropped (its
// destination lowers to null), leaving the resulting fat pointer's vtable half uninitialized.
TEST_CASE("A `^dyn I` coercion built inside an imported module's function has a real vtable") {
    constexpr std::string_view IFACE{R"(
        pub const Shape := interface {
            pub const area := fn(&self): i32;
        };
    )"};
    constexpr std::string_view HELPER{R"(
        import "iface.gh" as iface;
        const Box := struct { side: i32 };
        impl iface.Shape for Box {
            pub const area := fn(&self): i32 { return self.side * self.side; };
        }
        var box_impl: Box = .{ .side = 3 };
        pub const get_shape := fn(): ^dyn iface.Shape {
            return ^mut box_impl;
        };
    )"};
    CHECK(helpers::compile_and_run(
              R"(
        import "iface.gh" as iface;
        import "helper.gh" as helper;
        pub const main := fn(): i32 {
            const s := helper.get_shape();
            return s.area();
        };
    )",
              {
                  mock_file{"iface.gh", IFACE, "iface"},
                  mock_file{"helper.gh", HELPER, "helper"},
              }) == 9);
}

// FIXED: `gir::const_eval::coerce_dyn` registered its synthesised `__vtable.<N>` global on
// `module_` (whichever module is currently being folded) - wrong for the same reason as
// `emit_dyn_coercion` above, and hit here via a *different* path: computing a `var`/aggregate-
// `const` global's own constant initializer (`emitter::emit_top_level_decl`) runs with `module_`
// set to whichever module declares it, not the root. A `var` global whose struct-literal
// initializer coerces a field to `^dyn I` (`std.heap.page_allocator`'s own real shape) used to
// silently get an uninitialized vtable half the same way. Fixed via an optional
// `const_eval::set_vtable_root_module`, which `emitter` now sets once to its own fixed root
// reference at construction (`coerce_dyn` prefers it over `module_` when set).
TEST_CASE("A `var` global's struct field can coerce to `^dyn I` in its own initializer, "
         "cross-module") {
    constexpr std::string_view HELPER{R"(
        pub const Shape := interface {
            pub const area := fn(&self): i32;
        };
        const Box := struct { side: i32 };
        impl Shape for Box {
            pub const area := fn(&self): i32 { return self.side * self.side; };
        }
        const Holder := struct { pub s: ^dyn Shape };
        var box_impl: Box = .{ .side = 3 };
        pub var holder: Holder = .{ .s = ^mut box_impl };
    )"};
    CHECK(helpers::compile_and_run(
              R"(
        import "helper.gh" as helper;
        pub const main := fn(): i32 {
            return helper.holder.s.area();
        };
    )",
              {mock_file{"helper.gh", HELPER, "helper"}}) == 9);
}

} // namespace ghoti::tests
