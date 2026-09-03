#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("an inherent impl method runs on an instance") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        impl Point {
            pub const sum := fn(&self): i32 { return self.x + self.y; };
        }
        pub const main := fn(): i32 {
            var p := Point{ .x = 12, .y = 30 };
            return p.sum();
        };
    )") == 42);
}

TEST_CASE("an inherent impl static constructor runs via implicit access") {
    CHECK(helpers::compile_and_run(R"(
        const Box := struct { v: i32 };
        impl Box {
            pub const of := fn(v: i32): @this() { return .{ .v = v }; };
        }
        pub const main := fn(): i32 {
            const b := Box.of(7);
            return b.v;
        };
    )") == 7);
}

TEST_CASE("a trait impl method runs through static dispatch") {
    CHECK(helpers::compile_and_run(R"(
        const Doubler := interface {
            pub const apply := fn(&self, n: i32): i32;
        };
        const Twice := struct { factor: i32 };
        impl Doubler for Twice {
            pub const apply := fn(&self, n: i32): i32 { return n * self.factor; };
        }
        pub const main := fn(): i32 {
            var t := Twice{ .factor = 3 };
            return t.apply(14);
        };
    )") == 42);
}

TEST_CASE("a trait impl inherits and runs an interface default method") {
    CHECK(helpers::compile_and_run(R"(
        const Adder := interface {
            pub const step := fn(&self): i32;
            pub const stepThrice := fn(&self): i32 { return self.step() + self.step() + self.step(); };
        };
        const One := struct { by: i32 };
        impl Adder for One {
            pub const step := fn(&self): i32 { return self.by; };
        }
        pub const main := fn(): i32 {
            var o := One{ .by = 14 };
            return o.stepThrice();
        };
    )") == 42);
}

TEST_CASE("a type can implement two different interfaces") {
    CHECK(helpers::compile_and_run(R"(
        const Reader := interface { pub const rd := fn(&self): i32; };
        const Writer := interface { pub const wr := fn(&self): i32; };
        const Dev := struct { r: i32, w: i32 };
        impl Reader for Dev { pub const rd := fn(&self): i32 { return self.r; }; }
        impl Writer for Dev { pub const wr := fn(&self): i32 { return self.w; }; }
        pub const main := fn(): i32 {
            var d := Dev{ .r = 20, .w = 22 };
            return d.rd() + d.wr();
        };
    )") == 42);
}

TEST_CASE("an `impl I` bounded generic parameter dispatches to the argument's impl") {
    CHECK(helpers::compile_and_run(R"(
        const Doubler := interface { pub const apply := fn(&self, n: i32): i32; };
        const Twice := struct { k: i32 };
        impl Doubler for Twice { pub const apply := fn(&self, n: i32): i32 { return n * self.k; }; }
        const run := fn(d: &impl Doubler, n: i32): i32 { return d.apply(n); };
        pub const main := fn(): i32 {
            var t := Twice{ .k = 3 };
            return run(&t, 14);
        };
    )") == 42);
}

TEST_CASE("an `impl (A + B)` intersection parameter uses both interfaces") {
    CHECK(helpers::compile_and_run(R"(
        const R := interface { pub const rd := fn(&self): i32; };
        const W := interface { pub const wr := fn(&self): i32; };
        const Dev := struct { a: i32, b: i32 };
        impl R for Dev { pub const rd := fn(&self): i32 { return self.a; }; }
        impl W for Dev { pub const wr := fn(&self): i32 { return self.b; }; }
        const tee := fn(x: &impl (R + W)): i32 { return x.rd() + x.wr(); };
        pub const main := fn(): i32 {
            var d := Dev{ .a = 19, .b = 23 };
            return tee(&d);
        };
    )") == 42);
}

TEST_CASE("a parameterized inherent impl adds a method to every instantiation of its ctor") {
    CHECK(helpers::compile_and_run(R"(
        const Box := fn(T: type): type { return struct { val: T }; };
        impl(T: type) Box(T) {
            pub const doubled := fn(&self): T { return self.val + self.val; };
        }
        pub const main := fn(): i32 {
            var b: Box(i32) = .{ .val = 21 };
            return b.doubled();
        };
    )") == 42);
}

TEST_CASE("one parameterized-impl method set serves repeated uses of the same instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const Box := fn(T: type): type { return struct { val: T }; };
        impl(T: type) Box(T) {
            pub const get := fn(&self): T { return self.val; };
        }
        pub const main := fn(): i32 {
            var a: Box(i32) = .{ .val = 20 };
            var b: Box(i32) = .{ .val = 22 };
            return a.get() + b.get();
        };
    )") == 42);
}

TEST_CASE("a parameterized trait impl over a local ctor dispatches statically") {
    CHECK(helpers::compile_and_run(R"(
        const Show := interface { pub const show := fn(&self): i32; };
        const Box := fn(T: type): type { return struct { val: T }; };
        impl(T: type) Show for Box(T) {
            pub const show := fn(&self): i32 { return self.val; };
        }
        pub const main := fn(): i32 {
            var b: Box(i32) = .{ .val = 42 };
            return b.show();
        };
    )") == 42);
}

} // namespace ghoti::tests
