#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

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

TEST_CASE("a parameterized impl with two type parameters remaps each independently") {
    CHECK(helpers::compile_and_run(R"(
        const Pair := fn(A: type, B: type): type { return struct { a: A, b: B }; };
        impl(A: type, B: type) Pair(A, B) {
            pub const first := fn(&self): A { return self.a; };
            pub const second := fn(&self): B { return self.b; };
        }
        pub const main := fn(): i32 {
            var p: Pair(i32, i64) = .{ .a = 30, .b = 12 };
            return p.first() + @as(i32, p.second());
        };
    )") == 42);
}

TEST_CASE("a parameterized impl folds a `constexpr` parameter into its method bodies") {
    CHECK(helpers::compile_and_run(R"(
        const Ring := fn(constexpr sz: usize): type { return struct { head: usize }; };
        impl(constexpr n: usize) Ring(n) {
            pub const capacity := fn(&self): usize { return n; };
            pub const half := fn(&self): usize { return n / 2; };
        }
        pub const main := fn(): i32 {
            var r: Ring(28) = .{ .head = 0 };
            return @as(i32, r.capacity()) + @as(i32, r.half());
        };
    )") == 42);
}

TEST_CASE("a parameterized impl mixes a type and a `constexpr` parameter") {
    CHECK(helpers::compile_and_run(R"(
        const Slot := fn(T: type, constexpr tag: i32): type { return struct { val: T }; };
        impl(T: type, constexpr tag: i32) Slot(T, tag) {
            pub const tagged := fn(&self): T { return self.val + tag; };
        }
        pub const main := fn(): i32 {
            var s: Slot(i32, 2) = .{ .val = 40 };
            return s.tagged();
        };
    )") == 42);
}

TEST_CASE("a `constexpr` parameterized-impl param drives a runtime loop bound") {
    CHECK(helpers::compile_and_run(R"(
        const Counter := fn(constexpr limit: usize): type { return struct { base: i32 }; };
        impl(constexpr n: usize) Counter(n) {
            pub const upto := fn(&self): i32 {
                var acc: i32 = self.base;
                var i: usize = 0;
                while (i < n) { acc = acc + 1; i = i + 1; }
                return acc;
            };
        }
        pub const main := fn(): i32 {
            var c: Counter(9) = .{ .base = 33 };
            return c.upto();
        };
    )") == 42);
}

TEST_CASE("a parameterized impl in a library module is used from the consumer") {
    CHECK(helpers::compile_and_run(
              R"(
        import "shapes.gh" as shapes;
        pub const main := fn(): i32 {
            var s: shapes::Scaled(i32) = .{ .base = 42 };
            return s.size();
        };
    )",
              helpers::make_vector<helpers::mock_file>(helpers::mock_file{
                  "shapes.gh",
                  R"(
            pub const Scaled := fn(T: type): type { return struct { base: T }; };
            pub const Measure := interface { pub const size := fn(&self): i32; };
            impl(T: type) Measure for Scaled(T) {
                pub const size := fn(&self): i32 { return self.base; };
            }
        )",
                  "shapes",
              })) == 42);
}

} // namespace ghoti::tests
