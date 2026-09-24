#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

using helpers::mock_file;

TEST_CASE("a poisoned aggregate member does not crash a constexpr type-ctor instantiation") {
    helpers::expect_compile_error(R"(
        const Result := fn(T: type, E: type): type { return union { ok: T, err: E }; };
        constexpr R := fn(T: type): type { return Result(T, i32); };
        const S := struct {
            a: u32,
            const bad := fn(&self): u32 { return s.a; };
        };
        const g := fn(): R(S) { return .{ .ok = S{ .a = 1u32 } }; };
        pub const main := fn(): i32 { return 0; };
    )");
}

TEST_CASE("@This() resolves correctly inside a generic function's body") {
    CHECK(helpers::compile_and_run(R"(
        const S := struct {
            x: i32,

            const make := fn(v: auto): i32 {
                const r: @This() = S{ .x = v };
                return r.x;
            };
        };

        pub const main := fn(): i32 {
            return S.make(15);
        };
    )") == 15);
}

TEST_CASE("a static member fn returning its own struct type (with an array field)") {
    CHECK(helpers::compile_and_run(R"(
        const List := struct {
            buf: [4uz]mut i32,
            len: usize,

            const init := fn(): @This() {
                var l: @This() = undefined;
                l.len = 7uz;
                return l;
            };
            const size := fn(^self): i32 { return @intCast(i32, self.len); };
        };

        pub const main := fn(): i32 {
            const l := List.init();
            return l.size();
        };
    )") == 7);
}

TEST_CASE("a generic fn's body-local `const` decls are re-typed for each instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const dup := fn(T: type, x: T): T {
            const y := x;
            const z := y;
            return z;
        };
        pub const main := fn(): i32 {
            const a := dup(i32, 10);
            const b := dup(u8, 5u8);
            return a + @intCast(i32, b);
        };
    )") == 15);
}

TEST_CASE("the `?` operator works inside a generic function body") {
    CHECK(helpers::compile_and_run(R"(
        const R := union { ok: i32, err: u8 };
        impl builtin.Unwrappable for R {
            using Output = i32;
            using Residual = u8;
            pub const branch := fn(self): builtin.Flow(i32, u8) {
                return match (self) {
                    .ok => |v| builtin.Flow(i32, u8){ .@"continue" = v },
                    .err => |e| builtin.Flow(i32, u8){ .@"break" = e },
                };
            };
        }
        impl builtin.Rewrappable for R {
            using From = u8;
            pub const from_residual := fn(r: u8): @This() { return .{ .err = r }; };
        }
        const first := fn(T: type, r: R): R {
            const v := r?;
            return .{ .ok = v + 1 };
        };
        pub const main := fn(): i32 {
            const x := first(i32, .{ .ok = 7 });
            return match (x) { .ok => |v| v, .err => 0 };
        };
    )") == 8);
}

TEST_CASE("a `using` alias inside a generic body re-resolves per instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const Result := fn(T: type, E: type): type {
            return union {
                ok: T,
                err: E,

                pub constexpr mapErr := fn(&self, func: auto): auto {
                    constexpr fn_info := @typeInfo(@TypeOf(func));
                    const NewErr := fn_info.function.return_type;
                    using NewRes = Result(T, NewErr);
                    return match (self) {
                        .ok => |happy| NewRes{ .ok = happy },
                        .err => |sad| NewRes{ .err = func(sad) },
                    };
                };
            };
        };

        pub const main := fn(): i32 {
            const a: Result(void, i32) = .{ .err = 1 };
            const mapped_bool := a.mapErr(fn(val: i32): bool { return val != 0; });
            const bool_ok := match (mapped_bool) {
                .ok => false,
                .err => |e| e == true,
            };

            const toU8 := fn(_: i32): u8 { return 7; };
            const b: Result(void, i32) = .{ .err = 2 };
            const mapped_u8 := b.mapErr(toU8);
            const u8_ok := match (mapped_u8) {
                .ok => false,
                .err => |e| e == 7,
            };

            return if (bool_ok and u8_ok) 1 else 0;
        };
    )") == 1);
}

TEST_CASE("Multi-method struct with generic Result error checks compiles") {
    const auto exit_code{helpers::compile_and_run(R"(
        const Result := fn(T: type, E: type): type { return union { ok: T, err: E }; };
        const Error := enum : i32 { none, fail, _ };

        const File := struct {
            fd: i32,
            pub const open := fn(path: i32): Result(File, Error) {
                return if (path > 0) .{ .ok = File{ .fd = path } } else .{ .err = .fail };
            };
            pub const read := fn(&self): Result(i32, Error) {
                return .{ .ok = self.fd };
            };
            pub const write := fn(&mut self, v: i32): Result(bool, Error) {
                self.fd = v;
                return .{ .ok = true };
            };
        };

        pub const main := fn(): i32 {
            var f1 := File.open(7);
            return match (f1) {
                .ok => |f| {
                    const r := f.read();
                    return match (r) { .ok => |v| v, .err => 0 };
                },
                .err => 0,
            };
        };
    )")};
    CHECK(exit_code == 7);
}

TEST_CASE("generic member functions in an inherent impl block on a non-generic struct") {
    CHECK(helpers::compile_and_run(R"(
        const Processor := struct {
            factor: i32,
        };
        impl Processor {
            pub const identity := fn(T: type, x: T): T {
                return x;
            };
            pub const apply := fn(&self, U: type, val: U): i32 {
                return self.factor + @intCast(i32, val);
            };
            pub const scale := fn(&self, constexpr multiplier: i32, val: i32): i32 {
                return (self.factor + val) * multiplier;
            };
            pub const process_auto := fn(&self, val: auto): i32 {
                return self.factor * @intCast(i32, val);
            };
        }

        pub const main := fn(): i32 {
            const p := Processor{ .factor = 2 };
            const a := Processor.identity(i32, 5);
            const b := Processor.identity(u8, 3u8);
            const c := p.apply(i32, 4);
            const d := p.apply(u8, 2u8);
            const e := p.scale(3, 4);
            const f := p.scale(2, 4);
            const g := p.process_auto(5i32);
            const h := p.process_auto(4u8);
            return a + @intCast(i32, b) + c + d + e + f + g + h;
        };
    )") == 5 + 3 + 6 + 4 + 18 + 12 + 10 + 8);
}

TEST_CASE("generic member functions in an inherent impl block on a generic aggregate") {
    CHECK(helpers::compile_and_run(R"(
        const Container := fn(T: type): type {
            return struct {
                val: T,
            };
        };
        impl(T: type) Container(T) {
            pub const map := fn(&self, U: type, mapper: fn(x: T): U): Container(U) {
                return .{ .val = mapper(self.val) };
            };
            pub const fold := fn(&self, Acc: type, init_val: Acc, f: fn(acc: Acc, item: T): Acc): Acc {
                return f(init_val, self.val);
            };
            pub const repeat := fn(&self, constexpr N: i32): i32 {
                return @intCast(i32, self.val) * N;
            };
        }

        pub const main := fn(): i32 {
            const c1: Container(i32) = .{ .val = 10 };
            const dbl := fn(x: i32): i64 { return @intCast(i64, x * 2); };
            const is_gt := fn(x: i32): bool { return x > 5; };
            const c2 := c1.map(i64, dbl);
            const c3 := c1.map(bool, is_gt);

            const add_acc := fn(acc: i32, item: i32): i32 { return acc + item; };
            const folded := c1.fold(i32, 5, add_acc);

            const r1 := c1.repeat(3);
            const r2 := c1.repeat(2);

            const b2: i32 = if (c3.val) 1 else 0;
            return @intCast(i32, c2.val) + b2 + folded + r1 + r2;
        };
    )") == 20 + 1 + 15 + 30 + 20);
}

TEST_CASE("generic member functions in union and enum impl blocks") {
    CHECK(helpers::compile_and_run(R"(
        const Opt := union {
            val: i32,
            none: void,
        };
        impl Opt {
            pub const map_or := fn(&self, U: type, default_val: U, f: fn(x: i32): U): U {
                return match (*self) {
                    .val => |v| f(v),
                    .none => default_val,
                };
            };
        }

        const Status := enum : i32 {
            ok,
            err,
            _,
        };
        impl Status {
            pub const select := fn(&self, T: type, on_ok: T, on_err: T): T {
                return match (*self) {
                    .ok => on_ok,
                    .err => on_err,
                    _ => on_err,
                };
            };
        }

        pub const main := fn(): i32 {
            const o1: Opt = .{ .val = 7 };
            const o2: Opt = .{ .none = {} };
            const add_one := fn(x: i32): i32 { return x + 1; };
            const to_i64 := fn(x: i32): i64 { return @intCast(i64, x * 2); };

            const v1 := o1.map_or(i32, 0, add_one);
            const v2 := o2.map_or(i32, 10, add_one);
            const v3 := o1.map_or(i64, 0i64, to_i64);

            const s1: Status = .ok;
            const s2: Status = .err;
            const sel1 := s1.select(i32, 10, 20);
            const sel2 := s2.select(i32, 10, 20);
            const sel3 := s1.select(u8, 5u8, 15u8);

            return v1 + v2 + @intCast(i32, v3) + sel1 + sel2 + @intCast(i32, sel3);
        };
    )") == 8 + 10 + 14 + 10 + 20 + 5);
}

TEST_CASE("generic member functions defined inline inside aggregate definitions") {
    CHECK(helpers::compile_and_run(R"(
        const Item := struct {
            x: i32,
            pub const identity := fn(T: type, val: T): T {
                return val;
            };
            pub const add_val := fn(&self, U: type, val: U): i32 {
                return self.x + @intCast(i32, val);
            };
        };

        const TaggedUnion := union {
            num: i32,
            pub const get_or := fn(&self, T: type, fallback: T): T {
                return fallback;
            };
        };

        const DirectEnum := enum : i32 {
            a,
            b,
            _,
            pub const choose := fn(&self, T: type, x: T, y: T): T {
                return match (*self) {
                    .a => x,
                    .b => y,
                    _ => y,
                };
            };
        };

        pub const main := fn(): i32 {
            const item := Item{ .x = 5 };
            const id1 := Item.identity(i32, 10);
            const id2 := Item.identity(u8, 5u8);
            const a1 := item.add_val(i32, 5);
            const a2 := item.add_val(u8, 3u8);

            const tu: TaggedUnion = .{ .num = 42 };
            const g1 := tu.get_or(i32, 20);
            const g2 := tu.get_or(u8, 7u8);

            const de: DirectEnum = .a;
            const c1 := de.choose(i32, 1, 2);
            const c2 := de.choose(u8, 10u8, 20u8);

            return id1 + @intCast(i32, id2) + a1 + a2 + g1 + @intCast(i32, g2) + c1 + @intCast(i32, c2);
        };
    )") == 10 + 5 + 10 + 8 + 20 + 7 + 1 + 10);
}

TEST_CASE("aggregate member function implementation calling a local generic function") {
    CHECK(helpers::compile_and_run(R"(
        const Engine := struct {
            base: i32,
        };
        impl Engine {
            pub const run := fn(&self, x: i32, y: u8): i32 {
                const dup := fn(T: type, val: T): T {
                    return val + val;
                };
                const r1 := dup(i32, x);
                const r2 := dup(u8, y);
                return self.base + r1 + @intCast(i32, r2);
            };
        }

        pub const main := fn(): i32 {
            const eng := Engine{ .base = 20 };
            return eng.run(10, 5u8);
        };
    )") == 50);
}

TEST_CASE("cross-module aggregate impl with generic member functions") {
    constexpr std::string_view math_gh{R"(
        pub const Box := struct {
            value: i32,
        };
        impl Box {
            pub const transform := fn(&self, T: type, f: fn(x: i32): T): T {
                return f(self.value);
            };
            pub const scale_static := fn(T: type, x: T, factor: i32): i32 {
                return @intCast(i32, x) * factor;
            };
        }
    )"};

    const auto exit_code{helpers::compile_and_run(
        R"(
            import "math.gh" as math;
            pub const main := fn(): i32 {
                const b := math.Box{ .value = 6 };
                const to_i64 := fn(x: i32): i64 { return @intCast(i64, x * 2); };
                const to_i32 := fn(x: i32): i32 { return x + 8; };

                const v1 := b.transform(i64, to_i64);
                const v2 := b.transform(i32, to_i32);
                const s1 := math.Box.scale_static(i32, 10, 3);
                const s2 := math.Box.scale_static(u8, 4u8, 5);

                return @intCast(i32, v1) + v2 + s1 + s2;
            };
        )",
        {
            mock_file{"math.gh", math_gh, "math"},
        })};
    CHECK(exit_code == 12 + 14 + 30 + 20);
}

TEST_CASE("a value parameter typed via an earlier `T: type` parameter infers a single-arg "
          "builtin's target type from T's bound value") {
    CHECK(helpers::compile_and_run(R"(
        const add := fn(T: type, x: T, y: T): T { return x + y; };
        pub const main := fn(): i32 {
            const a := add(i32, 10, 4);
            const b := add(u8, 5u8, @intCast(a));
            return a + @intCast(i32, b);
        };
    )") == 33);
}

TEST_CASE("a value parameter typed via an earlier `T: type` parameter rejects an argument that "
          "doesn't fit T's bound value, instead of miscompiling") {
    helpers::expect_compile_error(R"(
        const add := fn(T: type, x: T, y: T): T { return x + y; };
        pub const main := fn(): i32 {
            const a := add(i32, 10, 4);
            const b := add(u8, 5u8, a);
            return a + @intCast(i32, b);
        };
    )");
}

TEST_CASE("a generic's body-local annotated decl is re-typed for each instantiation") {
    SECTION("`var a: @TypeOf(value)` with two `auto` widths") {
        CHECK(helpers::compile_and_run(R"(
            const f := fn(value: auto): u8 {
                var a: @TypeOf(value) = value;
                return @intCast(a % 3);
            };
            pub const main := fn(): i32 {
                return @as(i32, f(5u16)) + @as(i32, f(70000u32));   // 2 + 1
            };
        )") == 3);
    }

    SECTION("`var a: T` with two `T: type` arguments") {
        CHECK(helpers::compile_and_run(R"(
            const f := fn(T: type, value: T): u8 {
                var a: T = value;
                return @intCast(a % 3);
            };
            pub const main := fn(): i32 {
                return @as(i32, f(u16, 5)) + @as(i32, f(u32, 70000));   // 2 + 1
            };
        )") == 3);
    }

    SECTION("a `using` alias sized from `@typeInfo(@TypeOf(value))`") {
        CHECK(helpers::compile_and_run(R"(
            const f := fn(value: auto, base: u8): u8 {
                constexpr info := @typeInfo(@TypeOf(value)).int;
                constexpr bits := @max(info.bits, 8u16);
                using MinInt = @Int(.{ .signedness = .unsigned, .bits = bits });
                var a: MinInt = value;
                const d := a % @intCast(MinInt, base);
                return @intCast(d);
            };
            pub const main := fn(): i32 {
                return @as(i32, f(5u16, 2)) + @as(i32, f(70003u32, 4));   // 1 + 3
            };
        )") == 4);
    }

    SECTION("a `[n]T` local keeps each instantiation's own element type") {
        CHECK(helpers::compile_and_run(R"(
            const f := fn(T: type, value: T): i32 {
                var buf: [2]mut T = undefined;
                buf[0] = value;
                buf[1] = value;
                return @intCast(buf[0] + buf[1]);
            };
            pub const main := fn(): i32 { return f(u8, 3) + f(i64, 20); };
        )") == 46);
    }
}

TEST_CASE("a generic `fn(T: type, a: []T, b: []T)` call correctly types both slice arguments "
          "when one comes from `@typeInfo` reflection") {
    CHECK(helpers::compile_and_run_tests(R"(
        pub constexpr eql := fn(T: type, a: []T, b: []T): bool {
            if (a.len != b.len) return false;
            for (a, b) |a_elem, b_elem| { if (a_elem != b_elem) return false; }
            return true;
        };

        const Data := struct { count_x: u32 };

        test "field name from reflection compares correctly against another []u8 argument" {
            const needle: []u8 = "count_x"[0..];
            for constexpr (@typeInfo(Data).@"struct".fields) |field| {
                @expect(eql(u8, field.name, needle));
            }
        }
    )") == 0);
}

TEST_CASE("`@TypeOf(x)` nested in a compound type annotation denotes the type, not `type`") {
    SECTION("pointer, slice, and array annotations in a non-generic body") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                var v: u8 = 3;
                const p: ^mut @TypeOf(v) = ^mut v;
                var arr: [2]mut u8 = .{ 4, 5 };
                const s: []mut @TypeOf(v) = arr[0..];
                const b: [2]@TypeOf(v) = .{ 6, 7 };
                return @as(i32, *p) + @as(i32, s[0]) + @as(i32, b[1]);
            };
        )") == 14);
    }

    SECTION("writes through `^mut @TypeOf(v)` and `&mut @TypeOf(v)` reach the original") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                var v: i32 = 1;
                const p: ^mut @TypeOf(v) = ^mut v;
                *p = *p + 10;
                const r: &mut @TypeOf(v) = &mut v;
                r = r + 100;
                return v;
            };
        )") == 111);
    }

    SECTION("a parameter typed `^@TypeOf(b)` from an earlier parameter") {
        CHECK(helpers::compile_and_run(R"(
            const f := fn(b: i32, c: ^@TypeOf(b)): i32 { return b + *c; };
            pub const main := fn(): i32 {
                const x: i32 = 40;
                return f(2, ^x);
            };
        )") == 42);
    }

    SECTION("each generic instantiation sees its own element type") {
        CHECK(helpers::compile_and_run(R"(
            const f := fn(T: type, value: T): i32 {
                var copy: T = value;
                const p: ^mut @TypeOf(copy) = ^mut copy;
                var buf: [2]mut @TypeOf(value) = .{ value, value };
                const s: []mut @TypeOf(value) = buf[0..];
                return @intCast(*p + s[1]);
            };
            pub const main := fn(): i32 { return f(u8, 3) + f(i64, 20); };
        )") == 46);
    }
}

} // namespace ghoti::tests
