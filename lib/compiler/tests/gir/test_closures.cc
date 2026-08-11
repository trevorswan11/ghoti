#include <string>

#include <catch2/catch_test_macros.hpp>

#include "helpers/gir.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("A non-capturing closure emits as a plain function, no environment") {
    auto       ctx_idx{helpers::resolve_and_check(R"(
        const outer := fn(): i32 {
            const add := fn(x: i32): i32 {
                return x;
            };
            return add(5);
        };
    )")};
    const auto dump{helpers::dump_gir(ctx_idx)};

    CHECK_FALSE(dump.contains("alloca closure"));
    CHECK_FALSE(dump.contains("self: &closure"));
}

TEST_CASE("A VALUE capture loads once at env-construction and once at call time") {
    auto       ctx_idx{helpers::resolve_and_check(R"(
        const outer := fn(): i32 {
            var offset: i32 = 10;
            const add := fn(x: i32): i32 {
                return x + offset;
            };
            return add(5);
        };
    )")};
    const auto dump{helpers::dump_gir(ctx_idx)};

    CHECK(dump.contains("fn closure"));
    CHECK(dump.contains("self: &closure"));
    CHECK(dump.contains("alloca closure"));
}

TEST_CASE("A MUT_REF capture loads the reference once and reads/writes through it") {
    auto       ctx_idx{helpers::resolve_and_check(R"(
        const outer := fn(): void {
            var counter: i32 = 0;
            const bump := fn(): void {
                counter = counter + 1;
            };
        };
    )")};
    const auto dump{dump_gir(ctx_idx)};

    CHECK(dump.contains("alloca closure"));
    CHECK(dump.contains("get_element_ptr &i32"));
    CHECK(dump.contains("fn closure"));
    CHECK(dump.contains("self: &closure"));
}

TEST_CASE("Capturing across two function boundaries is rejected, not silently miscompiled") {
    auto [ctx, idx]{helpers::resolve(
        "const outer := fn(): void { var offset: i32 = 0; const middle := fn(): void { "
        "const inner := fn(x: i32): i32 { return x + offset; }; }; };")};
    CHECK(ctx->root_mod.is_poisoned());
}

} // namespace ghoti::tests
