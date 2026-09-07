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
            const size := fn(^self): i32 { return @as(i32, self.len); };
        };

        pub const main := fn(): i32 {
            const l := List.init();
            return l.size();
        };
    )") == 7);
}

} // namespace ghoti::tests
