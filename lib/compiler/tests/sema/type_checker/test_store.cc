#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Type checker store and assignment validation") {
    SECTION("Valid variable assignment succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(): void {
                let mut x: i32 = 0;
                x = 42;
            };
        )");
    }

    SECTION("Assignment with incompatible type fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(): void {
                let mut x: i32 = 0;
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
                let mut x: opaque = undefined;
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
                let mut a: [4uz]i32 = [4uz]i32{0, 0, 0, 0};
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
                let arr: [3]u8 = .{1, 2, 3};
                let mut m: [3]mut u8 = arr;
                let mut n: [3]mut u8 = [3]u8{1, 2, 3};
                let mut k: [3]mut u8 = make();
                let h: [2][2]u8 = .{.{1, 2}, .{3, 4}};
                let mut g: [2]mut [2]mut u8 = h;
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
                let mut m: [3]mut u8 = .{1, 2, 3};
                let c: [3]u8 = m;
            };
        )");
    }

    SECTION("Const-element array argument binds a mutable-element array parameter") {
        helpers::type_check_and_verify(R"(
            const g := fn(a: [3]mut u8): u8 { return a[0]; };
            const f := fn(): void {
                let arr: [3]u8 = .{1, 2, 3};
                _ = g(arr);
            };
        )");
    }

    SECTION("Array copy still rejects gaining pointee mutability through pointer elements") {
        helpers::test_checker_fail(
            R"(
            const f := fn(p: ^u8): void {
                let arr: [2]^u8 = .{p, p};
                let mut m: [2]mut ^mut u8 = arr;
            };
        )",
            sema::diagnostic{"Type mismatch in store: cannot assign '[2]^u8' to '[2]mut ^mut u8'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{3UZ, 44UZ}});
    }

    SECTION("Array copy still rejects gaining mutability through slice elements") {
        helpers::test_checker_fail(
            R"(
            const f := fn(s: []u8): void {
                let arr: [2][]u8 = .{s, s};
                let mut m: [2][]mut u8 = arr;
            };
        )",
            sema::diagnostic{"Type mismatch in store: cannot assign '[2][]u8' to '[2][]mut u8'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{3UZ, 41UZ}});
    }

    SECTION("Const-element array still does not coerce to a mutable-element slice") {
        helpers::test_checker_fail(
            R"(
            const f := fn(): void {
                let mut arr: [3]u8 = .{1, 2, 3};
                let s: []mut u8 = arr;
            };
        )",
            sema::diagnostic{"Type mismatch in store: cannot assign '[]u8' to '[]mut u8'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{3UZ, 16UZ}});
    }
    SECTION("A reference where a pointer is expected is rejected with a `^` hint") {
        const auto expect_ref_to_ptr = [](std::string_view src, usize line, usize col, bool hint) {
            helpers::test_checker_fail(
                src,
                sema::diagnostic{hint ? "A reference does not implicitly convert to a pointer "
                                        "(did you mean `^` instead of `&`?)"
                                      : "A reference does not implicitly convert to a pointer",
                                 sema::error::TYPE_MISMATCH,
                                 std::pair{line, col}});
        };

        SECTION("`let` and `let mut` declarations") {
            expect_ref_to_ptr(R"(
            const f := fn(): void {
                let mut x: i32 = 0;
                let p: ^mut i32 = &mut x;
            };
        )",
                              3,
                              34,
                              true);
            expect_ref_to_ptr(R"(
            const f := fn(): void {
                let mut x: i32 = 0;
                let mut p: ^i32 = &x;
            };
        )",
                              3,
                              34,
                              true);
        }

        SECTION("Assignment, call argument, and struct field initializer") {
            expect_ref_to_ptr(R"(
            const f := fn(p: ^mut i32): void {
                let mut x: i32 = 0;
                let mut q: ^i32 = p;
                q = &x;
            };
        )",
                              4,
                              20,
                              true);
            expect_ref_to_ptr(R"(
            const g := fn(q: ^i32): i32 { return *q; };
            const f := fn(): i32 {
                let x: i32 = 0;
                return g(&x);
            };
        )",
                              4,
                              25,
                              true);
            expect_ref_to_ptr(R"(
            const S := struct { p: ^i32 };
            const f := fn(): void {
                let x: i32 = 0;
                let s: S = .{ .p = &x };
            };
        )",
                              4,
                              35,
                              true);
        }

        SECTION("A reference-typed value that isn't a `&` expression gets no `^` hint") {
            expect_ref_to_ptr(R"(
            const f := fn(x: &i32): ^i32 { return x; };
        )",
                              1,
                              50,
                              false);
        }
    }
}

} // namespace ghoti::tests
