#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Type checker store and assignment validation") {
    SECTION("Valid variable assignment succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(): void {
                var x: i32 = 0;
                x = 42;
            };
        )");
    }

    SECTION("Assignment with incompatible type fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(): void {
                var x: i32 = 0;
                x = true;
            };
        )",
            sema::diagnostic{
                "Type mismatch in store: cannot assign 'bool' to 'i32' (conversion from 'bool' to "
                "'i32' maps false/true to 0/1; use @intFromBool for an explicit conversion)",
                sema::error::TYPE_MISMATCH,
                std::pair{3UZ, 20UZ}});
    }

    SECTION("Store through mutable pointer succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(p: ^mut i32): void {
                *p = 42;
            };
        )");
    }

    SECTION("Store through const pointer fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(p: ^i32): void {
                *p = 42;
            };
        )",
            sema::diagnostic{"Cannot assign to constant memory through pointer",
                             sema::error::ASSIGNMENT_TO_CONST,
                             std::pair{2UZ, 21UZ}});
    }

    SECTION("Store incompatible type through pointer fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(p: ^mut i32): void {
                *p = true;
            };
        )",
            sema::diagnostic{
                "Type mismatch in store: cannot assign 'bool' to 'i32' (conversion from 'bool' to "
                "'i32' maps false/true to 0/1; use @intFromBool for an explicit conversion)",
                sema::error::TYPE_MISMATCH,
                std::pair{2UZ, 21UZ}});
    }

    SECTION("Allocating opaque variable fails with ILLEGAL_OPAQUE_TYPE") {
        helpers::test_checker_fail(
            R"(
            const f := fn(): void {
                var x: opaque = undefined;
            };
        )",
            sema::diagnostic{"Cannot allocate variable of opaque type",
                             sema::error::ILLEGAL_OPAQUE_TYPE,
                             std::pair{2UZ, 16UZ}});
    }

    SECTION("Indexed store through a mutable pointer succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(p: ^mut i32): void {
                p[0] = 42;
            };
        )");
    }

    SECTION("Indexed store through a const pointer fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(p: ^i32): void {
                p[0] = 42;
            };
        )",
            sema::diagnostic{"Cannot assign to an element of a non-mutable array or slice",
                             sema::error::ASSIGNMENT_TO_CONST,
                             std::pair{2UZ, 23UZ}});
    }

    SECTION("Writing an element of a const-element array still fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(): void {
                var a: [4uz]i32 = [4uz]i32{0, 0, 0, 0};
                a[0] = 42;
            };
        )",
            sema::diagnostic{"Cannot assign to an element of a non-mutable array or slice",
                             sema::error::ASSIGNMENT_TO_CONST,
                             std::pair{3UZ, 23UZ}});
    }

    SECTION("Copying a const-element array into mutable-element storage succeeds") {
        helpers::type_check_and_verify(R"(
            const make := fn(): [3]u8 { return [3]u8{1, 2, 3}; };
            const f := fn(): void {
                const arr: [3]u8 = .{1, 2, 3};
                var m: [3]mut u8 = arr;
                var n: [3]mut u8 = [3]u8{1, 2, 3};
                var k: [3]mut u8 = make();
                const h: [2][2]u8 = .{.{1, 2}, .{3, 4}};
                var g: [2]mut [2]mut u8 = h;
                m[0] = 10;
                n[0] = 1;
                k[0] = 1;
                g[0][0] = 1;
                m = arr;
            };
        )");
    }

    SECTION("Copying a mutable-element array into const-element storage succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(): void {
                var m: [3]mut u8 = .{1, 2, 3};
                const c: [3]u8 = m;
            };
        )");
    }

    SECTION("Const-element array argument binds a mutable-element array parameter") {
        helpers::type_check_and_verify(R"(
            const g := fn(a: [3]mut u8): u8 { return a[0]; };
            const f := fn(): void {
                const arr: [3]u8 = .{1, 2, 3};
                _ = g(arr);
            };
        )");
    }

    SECTION("Array copy still rejects gaining pointee mutability through pointer elements") {
        helpers::test_checker_fail(
            R"(
            const f := fn(p: ^u8): void {
                const arr: [2]^u8 = .{p, p};
                var m: [2]mut ^mut u8 = arr;
            };
        )",
            sema::diagnostic{"Type mismatch in store: cannot assign 'array' to 'array'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{3UZ, 40UZ}});
    }

    SECTION("Array copy still rejects gaining mutability through slice elements") {
        helpers::test_checker_fail(
            R"(
            const f := fn(s: []u8): void {
                const arr: [2][]u8 = .{s, s};
                var m: [2][]mut u8 = arr;
            };
        )",
            sema::diagnostic{"Type mismatch in store: cannot assign 'array' to 'array'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{3UZ, 37UZ}});
    }

    SECTION("Const-element array still does not coerce to a mutable-element slice") {
        helpers::test_checker_fail(
            R"(
            const f := fn(): void {
                var arr: [3]u8 = .{1, 2, 3};
                const s: []mut u8 = arr;
            };
        )",
            sema::diagnostic{"Type mismatch in store: cannot assign 'slice' to 'slice'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{3UZ, 16UZ}});
    }
}

} // namespace ghoti::tests
