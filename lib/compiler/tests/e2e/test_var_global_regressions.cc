#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

using helpers::mock_file;

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
