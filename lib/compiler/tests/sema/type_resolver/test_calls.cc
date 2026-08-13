#include <catch2/catch_test_macros.hpp>

#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("A plain function still satisfies a fn(T): U generic parameter") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const apply := fn(T: type, val: T, func: fn(T): T): T {
            return func(val);
        };

        const double_it := fn(x: i32): i32 {
            return x * 2;
        };

        const test_fn := fn(): i32 {
            return apply(i32, 5, double_it);
        };
    )")};
}

TEST_CASE("A capturing closure satisfies a fn(T): U generic parameter") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const apply := fn(T: type, val: T, func: fn(T): T): T {
            return func(val);
        };

        const test_fn := fn(): i32 {
            var offset: i32 = 10;
            const add_offset := fn(x: i32): i32 {
                return x + offset;
            };
            return apply(i32, 5, add_offset);
        };
    )")};
}

TEST_CASE("A closure whose signature does not match the declared fn(T): U shape is rejected") {
    auto [ctx, idx]{helpers::resolve(R"(
        const apply := fn(T: type, val: T, func: fn(T): T): T {
            return func(val);
        };

        const test_fn := fn(): i32 {
            var offset: i32 = 10;
            const add := fn(x: i32, y: i32): i32 {
                return x + y + offset;
            };
            return apply(i32, 5, add);
        };
    )")};
    CHECK(ctx->root_mod.is_poisoned());
}

TEST_CASE("A non-generic-looking function with a plain fn(...) parameter still accepts a closure") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const apply_once := fn(func: fn(i32): i32): i32 {
            return func(5);
        };

        const test_fn := fn(): i32 {
            var offset: i32 = 10;
            const add_offset := fn(x: i32): i32 {
                return x + offset;
            };
            return apply_once(add_offset);
        };
    )")};
}

TEST_CASE("A non-generic-looking function with a plain fn(...) parameter still accepts a "
          "plain function") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const apply_once := fn(func: fn(i32): i32): i32 {
            return func(5);
        };

        const double_it := fn(x: i32): i32 {
            return x * 2;
        };

        const test_fn := fn(): i32 {
            return apply_once(double_it);
        };
    )")};
}

} // namespace ghoti::tests
