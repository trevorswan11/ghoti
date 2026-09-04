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

TEST_CASE("`if constexpr` on a parameterized-impl `constexpr` param folds per instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const Buf := fn(constexpr cap: usize): type { return struct { head: i32 }; };
        impl(constexpr n: usize) Buf(n) {
            pub const kind := fn(&self): i32 {
                if constexpr (n > 4) { return 100 + self.head; }
                return self.head;
            };
        }
        pub const main := fn(): i32 {
            var big: Buf(8) = .{ .head = 1 };
            var small: Buf(2) = .{ .head = 5 };
            return big.kind() + small.kind();
        };
    )") == 106);
}

TEST_CASE("`match constexpr` on a parameterized-impl `constexpr` param selects per instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const Buf := fn(constexpr cap: usize): type { return struct { head: i32 }; };
        impl(constexpr n: usize) Buf(n) {
            pub const bucket := fn(&self): i32 {
                return match constexpr (n) { 0 => 0, 1 => 10, _ => 99 } + self.head;
            };
        }
        pub const main := fn(): i32 {
            var one: Buf(1) = .{ .head = 2 };
            var many: Buf(7) = .{ .head = 0 };
            return one.bucket() + many.bucket();
        };
    )") == 111);
}

TEST_CASE("a dead `if constexpr` arm in a parameterized-impl body is never resolved") {
    // The `big` arm reads a field that does not exist; it must not be type-checked for
    // `Tag(false)`.
    CHECK(helpers::compile_and_run(R"(
        const Tag := fn(constexpr big: bool): type { return struct { v: i32 }; };
        impl(constexpr big: bool) Tag(big) {
            pub const describe := fn(&self): i32 {
                if constexpr (big) { return self.v + self.missing_field; }
                return self.v;
            };
        }
        pub const main := fn(): i32 {
            var s: Tag(false) = .{ .v = 42 };
            return s.describe();
        };
    )") == 42);
}

TEST_CASE("a dead `match constexpr` arm in a parameterized-impl body is never resolved") {
    CHECK(helpers::compile_and_run(R"(
        const Tag := fn(constexpr mode: i32): type { return struct { v: i32 }; };
        impl(constexpr mode: i32) Tag(mode) {
            pub const run := fn(&self): i32 {
                return match constexpr (mode) {
                    0 => self.v,
                    _ => self.v + self.only_when_nonzero,
                };
            };
        }
        pub const main := fn(): i32 {
            var s: Tag(0) = .{ .v = 42 };
            return s.run();
        };
    )") == 42);
}

TEST_CASE("an `impl (A + B + C)` intersection parameter accepts three interfaces") {
    CHECK(helpers::compile_and_run(R"(
        const A := interface { pub const a := fn(&self): i32; };
        const B := interface { pub const b := fn(&self): i32; };
        const C := interface { pub const c := fn(&self): i32; };
        const Dev := struct { n: i32 };
        impl A for Dev { pub const a := fn(&self): i32 { return self.n; }; }
        impl B for Dev { pub const b := fn(&self): i32 { return self.n * 2; }; }
        impl C for Dev { pub const c := fn(&self): i32 { return self.n * 3; }; }
        const sum := fn(x: &impl (A + B + C)): i32 { return x.a() + x.b() + x.c(); };
        pub const main := fn(): i32 {
            var d := Dev{ .n = 7 };
            return sum(&d);
        };
    )") == 42);
}

TEST_CASE("a method call through a `&dyn I` dispatches to the concrete impl") {
    CHECK(helpers::compile_and_run(R"(
        const W := interface { pub const val := fn(&self): i32; };
        const File := struct { fd: i32 };
        impl W for File { pub const val := fn(&self): i32 { return self.fd; }; }
        const use := fn(w: &dyn W): i32 { return w.val(); };
        pub const main := fn(): i32 {
            var f := File{ .fd = 42 };
            return use(&f);
        };
    )") == 42);
}

TEST_CASE("a `^dyn I` fat pointer carries a default method through its vtable") {
    CHECK(helpers::compile_and_run(R"(
        const Shape := interface {
            pub const area := fn(&self): i32;
            pub const scaled := fn(&self, k: i32): i32 { return self.area() * k; };
        };
        const Box := struct { w: i32, h: i32 };
        impl Shape for Box { pub const area := fn(&self): i32 { return self.w * self.h; }; }
        const total := fn(s: ^dyn Shape): i32 { return s.area() + s.scaled(2); };
        pub const main := fn(): i32 {
            var b := Box{ .w = 3, .h = 4 };
            return total(^b);
        };
    )") == 36);
}

TEST_CASE("`&dyn I` dispatches across a module boundary") {
    CHECK(helpers::compile_and_run(
              R"(
            import "sh.gh" as sh;
            const measure := fn(x: &dyn sh::Shape): i32 { return x.area(); };
            pub const main := fn(): i32 {
                var q: sh::Sq = .{ .s = 7 };
                return measure(&q) - 7;
            };
        )",
              {helpers::mock_file{
                  "sh.gh",
                  R"(pub const Shape := interface { pub const area := fn(&self): i32; };
                     pub const Sq := struct { s: i32 };
                     impl Shape for Sq { pub const area := fn(&self): i32 { return self.s * self.s; }; })",
                  "sh"}}) == 42);
}

TEST_CASE("`[]^dyn I` iterates a heterogeneous collection through the vtable") {
    CHECK(helpers::compile_and_run(R"(
        const N := interface { pub const v := fn(&self): i32; };
        const A := struct { a: i32 };
        impl N for A { pub const v := fn(&self): i32 { return self.a; }; }
        const sumAll := fn(xs: []^dyn N): i32 {
            var total: i32 = 0;
            for (xs) |x| { total = total + x.v(); }
            return total;
        };
        pub const main := fn(): i32 {
            var p := A{ .a = 10 };
            var q := A{ .a = 32 };
            var arr: [2]^dyn N = .{ ^p, ^q };
            return sumAll(arr[..]);
        };
    )") == 42);
}

TEST_CASE("`&dyn I(Assoc = T)` substitutes the associated type in method signatures") {
    CHECK(helpers::compile_and_run(R"(
        const Src := interface { Item: type; pub const first := fn(&self): Item; };
        const Box := struct { v: i32 };
        impl Src for Box {
            using Item = i32;
            pub const first := fn(&self): Item { return self.v; };
        }
        const takeFirst := fn(s: &dyn Src(Item = i32)): i32 { return s.first(); };
        pub const main := fn(): i32 {
            var b := Box{ .v = 42 };
            return takeFirst(&b);
        };
    )") == 42);
}

TEST_CASE("two impls of the same interface keep distinct methods under static and dyn dispatch") {
    CHECK(helpers::compile_and_run(R"(
        const N := interface { pub const get := fn(&self): i32; };
        const A := struct { x: i32 };
        const B := struct { y: i32 };
        impl N for A { pub const get := fn(&self): i32 { return self.x; }; }
        impl N for B { pub const get := fn(&self): i32 { return self.y * 10; }; }
        const dyn_get := fn(n: &dyn N): i32 { return n.get(); };
        pub const main := fn(): i32 {
            var a := A{ .x = 1 };
            var b := B{ .y = 2 };
            return a.get() + b.get() + dyn_get(&a) + dyn_get(&b);
        };
    )") == 42);
}

TEST_CASE("a `&dyn I` argument coerces from a plain `&mut T`") {
    CHECK(helpers::compile_and_run(R"(
        const N := interface { pub const bump := fn(&mut self): i32; };
        const Ctr := struct { n: i32 };
        impl N for Ctr { pub const bump := fn(&mut self): i32 { self.n = self.n + 1; return self.n; }; }
        const run := fn(x: &mut dyn N): i32 { return x.bump() + x.bump(); };
        pub const main := fn(): i32 {
            var c := Ctr{ .n = 19 };
            return run(&mut c);
        };
    )") == 41);
}

TEST_CASE("a `^dyn I` stored in a struct field dispatches when used later") {
    CHECK(helpers::compile_and_run(R"(
        const N := interface { pub const get := fn(&self): i32; };
        const Src := struct { v: i32 };
        impl N for Src { pub const get := fn(&self): i32 { return self.v; }; }
        const Holder := struct { inner: ^dyn N };
        const readHeld := fn(h: &Holder): i32 { return h.inner.get(); };
        pub const main := fn(): i32 {
            var s := Src{ .v = 42 };
            var h := Holder{ .inner = ^s };
            return readHeld(&h);
        };
    )") == 42);
}

TEST_CASE("several methods dispatch through one `&dyn I`") {
    CHECK(helpers::compile_and_run(R"(
        const Vec := interface {
            pub const x := fn(&self): i32;
            pub const y := fn(&self): i32;
            pub const z := fn(&self): i32;
        };
        const P := struct { a: i32, b: i32, c: i32 };
        impl Vec for P {
            pub const x := fn(&self): i32 { return self.a; };
            pub const y := fn(&self): i32 { return self.b; };
            pub const z := fn(&self): i32 { return self.c; };
        }
        const norm := fn(v: &dyn Vec): i32 { return v.x() + v.y() + v.z(); };
        pub const main := fn(): i32 {
            var p := P{ .a = 12, .b = 14, .c = 16 };
            return norm(&p);
        };
    )") == 42);
}

TEST_CASE("a `&dyn I` passes through two call layers") {
    CHECK(helpers::compile_and_run(R"(
        const N := interface { pub const get := fn(&self): i32; };
        const T := struct { v: i32 };
        impl N for T { pub const get := fn(&self): i32 { return self.v; }; }
        const inner := fn(n: &dyn N): i32 { return n.get(); };
        const outer := fn(n: &dyn N): i32 { return inner(n) + 1; };
        pub const main := fn(): i32 {
            var t := T{ .v = 41 };
            return outer(&t);
        };
    )") == 42);
}

TEST_CASE("a `&mut dyn I` method mutates state observed by a later `&dyn I` call") {
    CHECK(helpers::compile_and_run(R"(
        const Acc := interface {
            pub const add := fn(&mut self, n: i32): void;
            pub const total := fn(&self): i32;
        };
        const Sum := struct { s: i32 };
        impl Acc for Sum {
            pub const add := fn(&mut self, n: i32): void { self.s = self.s + n; };
            pub const total := fn(&self): i32 { return self.s; };
        }
        const fill := fn(a: &mut dyn Acc): void { a.add(20); a.add(22); };
        pub const main := fn(): i32 {
            var sum := Sum{ .s = 0 };
            fill(&mut sum);
            var view: &dyn Acc = &sum;
            return view.total();
        };
    )") == 42);
}

TEST_CASE("a `&dyn I` default method calls a required method through the same vtable") {
    CHECK(helpers::compile_and_run(R"(
        const Countable := interface {
            pub const count := fn(&self): i32;
            pub const isEmpty := fn(&self): i32 { if (self.count() == 0) { return 1; } return 0; };
        };
        const Bag := struct { n: i32 };
        impl Countable for Bag { pub const count := fn(&self): i32 { return self.n; }; }
        const check := fn(c: &dyn Countable): i32 { return c.count() * 10 + c.isEmpty(); };
        pub const main := fn(): i32 {
            var b := Bag{ .n = 4 };
            return check(&b) + 2;
        };
    )") == 42);
}

} // namespace ghoti::tests
