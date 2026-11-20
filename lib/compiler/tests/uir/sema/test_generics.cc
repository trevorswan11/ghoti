#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/uir_run.hh"

namespace ghoti::tests {

using sema::error;

TEST_CASE("Each binding of a generic function is one instance", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const id = fn(x: auto): auto { return x; };
        const shifted = fn(comptime by: i32, x: i32): i32 { return x + by; };
        pub const main = fn(): i32 {
            let a: i64 = 4;
            let wide = id(a);
            return id(1i32) + id(2i32) + shifted(10, 1) + shifted(10, 2);
        };
    )")};
    const auto gir{run.gir()};
    CHECK(gir.contains("fn id__i32(x: i32) -> i32"));
    CHECK(gir.contains("fn id__i64(x: i64) -> i64"));
    CHECK(gir.contains("fn shifted__i32_i32_cx10(x: i32) -> i32"));
    CHECK(gir.find("fn id__i32(") == gir.rfind("fn id__i32("));
}

TEST_CASE("A type constructor's instances are one type per argument list", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const Box = fn(comptime T: type): type {
            return struct {
                v: T,
                pub const get = fn(&self): T { return self.v; };
            };
        };
        const A = Box(u8);
        const B = Box(u8);
        const C = Box(i64);
        pub const main = fn(): i32 {
            let a: A = .{ .v = 1 };
            let b: B = .{ .v = 2 };
            let c: C = .{ .v = 3 };
            let wide = c.get();
            return @as(i32, a.get()) + @as(i32, b.get());
        };
    )")};
    CHECK(run.type_name("A") == "Box(u8)");
    CHECK(run.type_name("C") == "Box(i64)");
    const auto gir{run.gir()};
    CHECK(gir.contains("fn Box__u8.get("));
    CHECK(gir.contains("fn Box__i64.get("));
    CHECK(gir.find("fn Box__u8.get(") == gir.rfind("fn Box__u8.get("));
}

TEST_CASE("Q13: a parameterized impl's unusable body compiles until the method is used", "[uir]") {
    constexpr std::string_view IMPL{R"(
        const Box = fn(comptime T: type): type { return struct { v: T }; };
        impl(T: type) Box(T) {
            pub const bumped = fn(&self): T { return self.v + 1; };
        }
    )"};

    // `bool + 1` has no meaning, but nothing asks for `Box(bool).bumped`
    const auto unused{helpers::uir_accepted(std::string{IMPL} + R"(
        pub const main = fn(): i32 {
            let b: Box(bool) = .{ .v = true };
            let n: Box(i32) = .{ .v = 41 };
            return n.bumped();
        };
    )")};
    CHECK_FALSE(unused.gir().contains("Box__bool.bumped"));

    const auto used{helpers::uir_rejected(std::string{IMPL} + R"(
        pub const main = fn(): i32 {
            let b: Box(bool) = .{ .v = true };
            _ = b.bumped();
            return 0;
        };
    )")};
    CHECK(used.codes() == std::vector{error::OPERATOR_TYPE_MISMATCH});
}

TEST_CASE("An interface default method is instantiated for its implementing type", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const Adder = interface {
            pub const step = fn(&self): i32;
            pub const twice = fn(&self): i32 { return self.step() + self.step(); };
        };
        const One = struct { by: i32 };
        impl Adder for One { pub const step = fn(&self): i32 { return self.by; }; }
        pub const main = fn(): i32 {
            let o = One{ .by = 21 };
            return o.twice();
        };
    )")};
    const auto gir{run.gir()};
    CHECK(gir.contains("fn twice(self: &struct) -> i32"));
    CHECK(gir.contains("call @step(param.0)"));
}

TEST_CASE("An argument that doesn't implement an `impl` bound is reported", "[uir]") {
    const auto run{helpers::uir_rejected(R"(
        const Shape = interface { pub const area = fn(&self): i32; };
        const Sq = struct { s: i32 };
        const measure = fn(x: &impl Shape): i32 { return x.area(); };
        pub const main = fn(): i32 {
            let q = Sq{ .s = 3 };
            return measure(&q);
        };
    )")};
    CHECK(run.codes() == std::vector{error::UNSATISFIED_BOUND});
    CHECK(run.messages() ==
          std::vector<std::string>{"`Sq` does not implement `Shape` required by parameter `x`"});
}

TEST_CASE("A `&dyn I` coercion builds one vtable per implementing type", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const Shape = interface { pub const area = fn(&self): i32; };
        const Sq = struct { s: i32 };
        impl Shape for Sq { pub const area = fn(&self): i32 { return self.s * self.s; }; }
        const total = fn(x: &dyn Shape): i32 { return x.area(); };
        pub const main = fn(): i32 {
            let a = Sq{ .s = 2 };
            let b = Sq{ .s = 3 };
            return total(&a) + total(&b);
        };
    )")};
    const auto gir{run.gir()};
    CHECK(gir.contains("global_addr @__vtable.0"));
    CHECK_FALSE(gir.contains("__vtable.1"));
}

TEST_CASE("A `mut?` method's result takes the mutability of its object", "[uir]") {
    const auto run{helpers::uir_rejected(R"(
        const S = struct {
            v: i32,
            pub const at = fn(&mut? self): &mut? i32 { return &mut? self.v; };
        };
        pub const main = fn(): i32 {
            let mut a = S{ .v = 1 };
            let b = S{ .v = 2 };
            let ra = a.at();
            let rb = b.at();
            *ra = 5;
            *rb = 6;
            return 0;
        };
    )")};
    CHECK(run.codes() == std::vector{error::ASSIGNMENT_TO_CONST});
}

} // namespace ghoti::tests
