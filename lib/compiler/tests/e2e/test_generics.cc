#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

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

TEST_CASE("@this() resolves correctly inside a generic function's body") {
    CHECK(helpers::compile_and_run(R"(
        const S := struct {
            x: i32,

            const make := fn(v: auto): i32 {
                const r: @this() = S{ .x = v };
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

            const init := fn(): @this() {
                var l: @this() = undefined;
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
            pub const fromResidual := fn(r: u8): @this() { return .{ .err = r }; };
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

} // namespace ghoti::tests
