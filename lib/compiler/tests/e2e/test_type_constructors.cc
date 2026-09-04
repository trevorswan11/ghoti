#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("E2E: a generic `union` type constructor is usable as a return type") {
    CHECK(helpers::compile_and_run(R"(
        const Result := fn(T: type, E: type): type {
            return union { ok: T, err: E };
        };

        using IntOr = Result(i32, i32);

        const parse := fn(ok: bool): IntOr {
            if (ok) { return .{ .ok = 40 }; }
            return .{ .err = 1 };
        };

        pub const main := fn(): i32 {
            const a: IntOr = parse(true);
            const b: IntOr = parse(false);
            var acc: i32 = 0;
            match (a) { .ok => { acc = acc + a.ok; }, .err => { acc = acc + 100; } }
            match (b) { .ok => { acc = acc + 100; }, .err => { acc = acc + b.err; } }
            return acc;
        };
    )") == 41);
}

TEST_CASE("E2E: a later parameter and the return type depend on an earlier parameter's type") {
    CHECK(helpers::compile_and_run(R"(
        const pick := fn(a: auto, b: @typeOf(a)): @typeOf(b) {
            return a + b;
        };

        pub const main := fn(): i32 {
            const x: i32 = 7;
            return pick(x, 2);
        };
    )") == 9);
}

TEST_CASE("E2E: a later parameter's type is a type-constructor call over an earlier parameter") {
    CHECK(helpers::compile_and_run(R"(
        const Box := fn(T: type): type { return struct { val: T }; };

        const unbox := fn(a: auto, b: Box(@typeOf(a))): i32 {
            return b.val;
        };

        pub const main := fn(): i32 {
            const x: i32 = 0;
            const boxed: Box(i32) = .{ .val = 9 };
            return unbox(x, boxed);
        };
    )") == 9);
}

TEST_CASE("E2E: the return type may also be a type-constructor call over an earlier parameter") {
    CHECK(helpers::compile_and_run(R"(
        const Box := fn(T: type): type { return struct { val: T }; };

        const rewrap := fn(a: auto, b: Box(@typeOf(a))): @typeOf(b) {
            return b;
        };

        pub const main := fn(): i32 {
            const x: i32 = 0;
            const boxed: Box(i32) = .{ .val = 9 };
            const out := rewrap(x, boxed);
            return out.val;
        };
    )") == 9);
}

TEST_CASE("E2E: two structurally distinct instantiations of a generic `struct` constructor") {
    CHECK(helpers::compile_and_run(R"(
        const Box := fn(T: type): type { return struct { val: T }; };

        using BoxI = Box(i32);
        using BoxF = Box(f64);

        pub const main := fn(): i32 {
            const a: BoxI = .{ .val = 5 };
            const b: BoxF = .{ .val = 2.5 };
            return a.val + @as(i32, b.val);
        };
    )") == 7);
}

TEST_CASE("E2E: two instantiations of a generic `union` constructor, matched independently") {
    CHECK(helpers::compile_and_run(R"(
        const Option := fn(T: type): type { return union { some: T, none: void }; };

        using OptI = Option(i32);
        using OptB = Option(u8);

        pub const main := fn(): i32 {
            const x: OptI = .{ .some = 30 };
            const y: OptB = .{ .some = @as(u8, 12) };
            var acc: i32 = 0;
            match (x) { .some => { acc = acc + x.some; }, .none => {} }
            match (y) { .some => { acc = acc + @as(i32, y.some); }, .none => {} }
            return acc;
        };
    )") == 42);
}

TEST_CASE("E2E: repeated same-argument instantiation shares one type") {
    CHECK(helpers::compile_and_run(R"(
        const Box := fn(T: type): type { return struct { val: T }; };

        using B1 = Box(i32);
        using B2 = Box(i32);

        const relay := fn(b: B1): B2 { return b; };

        pub const main := fn(): i32 {
            const a: B1 = .{ .val = 20 };
            const b: B2 = relay(.{ .val = 22 });
            return a.val + b.val;
        };
    )") == 42);
}

TEST_CASE("E2E: a zero-parameter `fn(): type` that builds a fresh struct") {
    CHECK(helpers::compile_and_run(R"(
        const MakePoint := fn(): type { return struct { x: i32, y: i32 }; };
        using Point = MakePoint();

        pub const main := fn(): i32 {
            const p: Point = .{ .x = 3, .y = 4 };
            return p.x + p.y;
        };
    )") == 7);
}

TEST_CASE("E2E: a zero-parameter `fn(): type` that builds a fresh union") {
    CHECK(helpers::compile_and_run(R"(
        const MakeCell := fn(): type { return union { i: i32, f: f32 }; };
        using Cell = MakeCell();

        pub const main := fn(): i32 {
            const c: Cell = .{ .i = 99 };
            match (c) { .i => { return c.i; }, .f => { return 0; } }
        };
    )") == 99);
}

TEST_CASE("E2E: a `fn(bool): type` that selects between existing named types") {
    CHECK(helpers::compile_and_run(R"(
        const Wide := struct { a: i64, b: i64 };
        const Narrow := struct { a: i32 };

        const pick := fn(wide: bool): type {
            if (wide) { return Wide; }
            return Narrow;
        };

        using Chosen = pick(true);

        pub const main := fn(): i32 {
            const v: Chosen = .{ .a = 3, .b = 4 };
            return @as(i32, v.a + v.b);
        };
    )") == 7);
}

TEST_CASE("E2E: @sizeOf / @alignOf resolve a `fn(bool): type` alias selecting a primitive") {
    CHECK(helpers::compile_and_run(R"(
        const choose := fn(wide: bool): type {
            if (wide) { return i64; }
            return i32;
        };
        using T = choose(true);
        using U = choose(false);

        pub const main := fn(): i32 {
            return @as(i32, @sizeOf(T)) + @as(i32, @alignOf(T))
                 + @as(i32, @sizeOf(U)) + @as(i32, @alignOf(U));
        };
    )") == 8 + 8 + 4 + 4);
}

TEST_CASE("E2E: a single generic type constructor instantiation with a member function") {
    CHECK(helpers::compile_and_run(R"(
        const Vec := fn(T: type): type {
            return struct {
                item: T,
                const make := fn(v: T): @this() { return .{ .item = v }; };
            };
        };

        pub const main := fn(): i32 {
            const a := Vec(i32).make(41);
            return a.item + 1;
        };
    )") == 42);
}

TEST_CASE("E2E: two instantiations of a generic type constructor do not alias their methods") {
    CHECK(helpers::compile_and_run(R"(
        const Vec := fn(T: type): type {
            return struct {
                item: T,
                const make    := fn(v: T): @this() { return .{ .item = v }; };
                const doubled := fn(^self): T { return self.item + self.item; };
            };
        };

        using VI = Vec(i32);
        using VL = Vec(i64);

        pub const main := fn(): i32 {
            const a := VI.make(3);
            const b := VL.make(7);
            return @as(i32, a.doubled()) + @as(i32, b.doubled());   // 6 + 14
        };
    )") == 20);
}

TEST_CASE("E2E: a non-generic `fn(): type` result with member functions") {
    CHECK(helpers::compile_and_run(R"(
        const Make := fn(): type {
            return struct {
                item: i32,
                const of      := fn(v: i32): @this() { return .{ .item = v }; };
                const doubled := fn(^self): i32 { return self.item + self.item; };
            };
        };
        using M = Make();

        pub const main := fn(): i32 {
            const a := M.of(21);
            return a.doubled();
        };
    )") == 42);
}

TEST_CASE("E2E: type constructor members with `&mut self` and sibling-method calls") {
    CHECK(helpers::compile_and_run(R"(
        const Box := fn(T: type): type {
            return struct {
                v: T,
                const make  := fn(x: T): @this() { return .{ .v = x }; };
                const get   := fn(^self): T { return self.v; };
                const bump  := fn(&mut self, by: T): void { self.v = self.v + by; };
                const twice := fn(^self): T { return self.get() + self.get(); };
            };
        };
        using BI = Box(i32);

        pub const main := fn(): i32 {
            var b := BI.make(10);
            b.bump(5);
            return b.twice();   // (10 + 5) * 2
        };
    )") == 30);
}

TEST_CASE("E2E: a cross-module non-generic `fn(): type` with member functions") {
    constexpr std::string_view LIB{R"(
        pub const Make := fn(): type {
            return struct {
                item: i32,
                pub const of      := fn(v: i32): @this() { return .{ .item = v }; };
                pub const doubled := fn(^self): i32 { return self.item + self.item; };
            };
        };
    )"};
    const auto                 exit_code{helpers::compile_and_run(
        R"(
            import "lib.gh" as lib;
            using M = lib::Make();
            pub const main := fn(): i32 {
                const a := M.of(21);
                return a.doubled();
            };
        )",
        {helpers::mock_file{"lib.gh", LIB, "lib"}})};
    CHECK(exit_code == 42);
}

TEST_CASE("E2E: a cross-module generic type constructor, two instantiations, methods distinct") {
    constexpr std::string_view VEC{R"(
        pub const Vec := fn(T: type): type {
            return struct {
                item: T,
                pub const make    := fn(v: T): @this() { return .{ .item = v }; };
                pub const doubled := fn(^self): T { return self.item + self.item; };
            };
        };
    )"};
    const auto                 exit_code{helpers::compile_and_run(
        R"(
            import "vec.gh" as v;
            using VI = v::Vec(i32);
            using VL = v::Vec(i64);
            pub const main := fn(): i32 {
                const a := VI.make(3);
                const b := VL.make(7);
                return @as(i32, a.doubled()) + @as(i32, b.doubled());   // 6 + 14
            };
        )",
        {helpers::mock_file{"vec.gh", VEC, "v"}})};
    CHECK(exit_code == 20);
}

TEST_CASE("E2E: two instantiations of the same constructor passed to another generic") {
    CHECK(helpers::compile_and_run(R"(
        const Box := fn(T: type): type { return struct { v: T }; };
        const boxSize := fn(B: type): i32 { return @as(i32, @sizeOf(B)); };

        pub const main := fn(): i32 {
            return boxSize(Box(i32)) * 10 + boxSize(Box(i64));   // 4*10 + 8
        };
    )") == 48);
}

TEST_CASE("E2E: a generic type constructor without member functions still resolves") {
    CHECK(helpers::compile_and_run(R"(
        const Pair := fn(A: type, B: type): type { return struct { a: A, b: B }; };

        pub const main := fn(): i32 {
            const p: Pair(i32, i32) = .{ .a = 4, .b = 3 };
            return p.a + p.b;
        };
    )") == 7);
}

TEST_CASE("E2E: a type constructor's members read its `constexpr` value parameters") {
    SECTION("a scalar `constexpr` parameter") {
        CHECK(helpers::compile_and_run(R"(
            const Box := fn(T: type, constexpr tag: i32): type {
                return struct {
                    val: T,
                    pub const tagged := fn(&self): i32 { return self.val + tag; };
                };
            };

            using B = Box(i32, 100);

            pub const main := fn(): i32 {
                var b: B = .{ .val = 5 };
                return b.tagged();
            };
        )") == 105);
    }

    SECTION("a `constexpr` function-value parameter") {
        CHECK(helpers::compile_and_run(R"(
            const dbl := fn(x: i32): i32 { return x * 2; };

            const Wrap := fn(T: type, constexpr f: fn(x: i32): i32): type {
                return struct {
                    val: T,
                    pub const apply := fn(&self): i32 { return f(self.val); };
                };
            };

            using W = Wrap(i32, dbl);

            pub const main := fn(): i32 {
                var w: W = .{ .val = 21 };
                return w.apply();
            };
        )") == 42);
    }

    SECTION("two instantiations keep distinct constexpr values") {
        CHECK(helpers::compile_and_run(R"(
            const Box := fn(T: type, constexpr tag: i32): type {
                return struct {
                    val: T,
                    pub const tagged := fn(&self): i32 { return self.val + tag; };
                };
            };

            using B10 = Box(i32, 10);
            using B20 = Box(i32, 20);

            pub const main := fn(): i32 {
                var a: B10 = .{ .val = 1 };
                var b: B20 = .{ .val = 1 };
                return a.tagged() + b.tagged();
            };
        )") == 11 + 21);
    }
}

TEST_CASE("E2E: a type constructor member sizes a local `[n]T` from a `constexpr` parameter") {
    SECTION("the array length folds from the constructor's `constexpr` binding") {
        CHECK(helpers::compile_and_run(R"(
            const Vec := fn(constexpr n: usize): type {
                return struct {
                    head: i32,
                    const probe := fn(&self): i32 {
                        var buf: [n]i32 = undefined;
                        const b0: i32 = buf[0];
                        return b0 - b0 + self.head + @as(i32, n);
                    };
                };
            };

            pub const main := fn(): i32 {
                var v: Vec(9) = .{ .head = 33 };
                return v.probe();
            };
        )") == 42);
    }

    SECTION("a `[n]mut T` local is writable and reads back what was stored") {
        CHECK(helpers::compile_and_run(R"(
            const Vec := fn(constexpr n: usize): type {
                return struct {
                    head: i32,
                    const sum := fn(&self): i32 {
                        var buf: [n]mut i32 = undefined;
                        var i: usize = 0;
                        while (i < n) { buf[i] = @as(i32, i) * 2; i = i + 1; }
                        var acc: i32 = 0;
                        var j: usize = 0;
                        while (j < n) { acc = acc + buf[j]; j = j + 1; }
                        return acc + self.head;
                    };
                };
            };

            pub const main := fn(): i32 {
                var v: Vec(5) = .{ .head = 3 };
                return v.sum();   // (0+2+4+6+8) + 3
            };
        )") == 23);
    }

    SECTION("two instantiations keep independent array lengths") {
        CHECK(helpers::compile_and_run(R"(
            const Vec := fn(constexpr n: usize): type {
                return struct {
                    pad: i32,
                    const fill := fn(&self): i32 {
                        var buf: [n]mut i32 = undefined;
                        var i: usize = 0;
                        while (i < n) : (i += 1) { buf[i] = @as(i32, i); }
                        var acc: i32 = 0;
                        var j: usize = 0;
                        while (j < n) : (j += 1) { acc = acc + buf[j]; }
                        return acc;
                    };
                };
            };

            pub const main := fn(): i32 {
                var a: Vec(3) = .{ .pad = 0 };
                var b: Vec(5) = .{ .pad = 0 };
                return a.fill() + b.fill();   // (0+1+2) + (0+1+2+3+4)
            };
        )") == 3 + 10);
    }
}

TEST_CASE("E2E: a plain generic fn sizes a local `[n]mut T` from a `constexpr` parameter") {
    CHECK(helpers::compile_and_run(R"(
        const probe := fn(constexpr n: usize, head: i32): i32 {
            var buf: [n]mut i32 = undefined;
            var i: usize = 0;
            while (i < n) { buf[i] = @as(i32, i) * 2; i = i + 1; }
            var acc: i32 = 0;
            var j: usize = 0;
            while (j < n) { acc = acc + buf[j]; j = j + 1; }
            return acc + head + @as(i32, n);
        };

        pub const main := fn(): i32 { return probe(5, 3); };   // (0+2+4+6+8) + 3 + 5
    )") == 28);
}

} // namespace ghoti::tests
