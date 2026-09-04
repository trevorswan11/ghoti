#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "ghoti/config.h"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("A non-C-ABI integer width is rejected in an extern struct field") {
    helpers::test_resolver_fail(
        "const S := extern struct { a: u100 };",
        sema::diagnostic{"extern struct field 'a' has type 'u100', which has no C ABI "
                         "representation; extern signatures accept 8/16/32/64-bit integers, "
                         "usize/isize, bool, f32, f64, f80",
                         sema::error::ILLEGAL_REFERENCE_FIELD,
                         std::pair{0UZ, 30UZ}});
}

TEST_CASE("A non-C-ABI integer width is rejected in an extern union field") {
    helpers::test_resolver_fail(
        "const U := extern union { b: u7 };",
        sema::diagnostic{"extern union field 'b' has type 'u7', which has no C ABI "
                         "representation; extern signatures accept 8/16/32/64-bit integers, "
                         "usize/isize, bool, f32, f64, f80",
                         sema::error::ILLEGAL_REFERENCE_FIELD,
                         std::pair{0UZ, 29UZ}});
}

TEST_CASE("A non-C-ABI parameter width is rejected in an extern fn signature") {
    helpers::test_resolver_fail(
        "extern const foo: fn(x: u3): void;",
        sema::diagnostic{"'fn(u3): void' has no C ABI representation; extern signatures accept "
                         "8/16/32/64-bit integers, usize/isize, bool, f32, f64, f80",
                         sema::error::ILLEGAL_REFERENCE_FIELD,
                         std::pair{0UZ, 18UZ}});
}

TEST_CASE("A non-C-ABI return width is rejected in an extern fn signature") {
    helpers::test_resolver_fail(
        "extern const bar: fn(): i17;",
        sema::diagnostic{"'fn(): i17' has no C ABI representation; extern signatures accept "
                         "8/16/32/64-bit integers, usize/isize, bool, f32, f64, f80",
                         sema::error::ILLEGAL_REFERENCE_FIELD,
                         std::pair{0UZ, 18UZ}});
}

TEST_CASE("f16 is rejected on an extern global") {
    helpers::test_resolver_fail(
        "extern var g: f16;",
        sema::diagnostic{"'f16' has no C ABI representation; extern signatures accept "
                         "8/16/32/64-bit integers, usize/isize, bool, f32, f64, f80",
                         sema::error::ILLEGAL_REFERENCE_FIELD,
                         std::pair{0UZ, 14UZ}});
}

TEST_CASE("f128 is rejected on an extern global") {
    helpers::test_resolver_fail(
        "extern var g: f128;",
        sema::diagnostic{"'f128' has no C ABI representation; extern signatures accept "
                         "8/16/32/64-bit integers, usize/isize, bool, f32, f64, f80",
                         sema::error::ILLEGAL_REFERENCE_FIELD,
                         std::pair{0UZ, 14UZ}});
}

TEST_CASE("C-ABI extern signatures and aggregates are accepted") {
    helpers::resolve_and_check("extern const ok: fn(x: u32, y: usize, z: isize): f64;");
    helpers::resolve_and_check("const S := extern struct { a: i64, b: bool };");
#if GHOTI_ASM_HOST_X86_64
    helpers::resolve_and_check("extern const with_f80: fn(x: f80): void;");
#endif
}

} // namespace ghoti::tests
