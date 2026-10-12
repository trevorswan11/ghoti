#include <catch2/catch_test_macros.hpp>

#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("A statement calling a noreturn function ends its path") {
    auto [ctx, idx]{helpers::type_check_and_verify(R"(
        extern("c", "exit") const exit: fn(code: c_int): noreturn;
        const stop = fn(): i32 {
            exit(15);
        };
        const pick = fn(b: bool): i32 {
            if (b) { return 1; }
            exit(2);
        };
    )")};
    CHECK_FALSE(ctx->root_mod.is_poisoned());
}

} // namespace ghoti::tests
