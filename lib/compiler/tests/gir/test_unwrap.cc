#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/gir.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("GIR `?` branches on the discriminant and emits a divergent return") {
    // `inner(x)` depends on a parameter, so the `?` cannot be constant-folded away.
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const R := union { ok: i32, err: i32 };
        const inner := fn(x: i32): R {
            return if (x > 0) R{ .ok = x }; else R{ .err = 1 };
        };
        const outer := fn(x: i32): R {
            const v := inner(x)?;
            return R{ .ok = v + 1 };
        };
    )")};

    const auto dump_text{helpers::dump_named_fn(*ctx, "outer")};
    CHECK(dump_text.find("cond_goto") != std::string::npos);
    CHECK(dump_text.find("ret") != dump_text.rfind("ret"));
}

TEST_CASE("GIR `!` guards the discriminant with a panic_handler call") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const O := union { some: i32, none: void };
        const grab := fn(o: O): i32 { return o!; };
    )")};

    const auto dump_text{helpers::dump_named_fn(*ctx, "grab")};
    CHECK(dump_text.find("cond_goto") != std::string::npos);
    CHECK(dump_text.find("panic_handler") != std::string::npos);
}

TEST_CASE("GIR reading a tagged-union field is discriminant-guarded") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const U := union { a: i32, b: i32 };
        const read_a := fn(u: U): i32 { return u.a; };
    )")};

    const auto dump_text{helpers::dump_named_fn(*ctx, "read_a")};
    CHECK(dump_text.find("cond_goto") != std::string::npos);
    CHECK(dump_text.find("panic_handler") != std::string::npos);
}

} // namespace ghoti::tests
