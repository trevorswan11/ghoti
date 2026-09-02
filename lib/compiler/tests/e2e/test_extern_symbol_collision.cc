#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

// Unreferenced, so DCE drops it before linking.
constexpr std::string_view SYS_MODULE{R"(
    extern("ghoti_fake_libc", "read") const raw_read: fn(n: i32): i32;

    pub const read := fn(fd: i32): i32 { return fd * 2; };
)"};

} // namespace

TEST_CASE("E2E: a function named like an `extern` link name in the same module") {
    CHECK(helpers::compile_and_run(R"(
        extern("ghoti_fake_libc", "write") const raw_write: fn(n: i32): i32;

        const write := fn(n: i32): i32 { return n + 1; };

        pub const main := fn(): i32 {
            return write(41);
        };
    )") == 42);
}

TEST_CASE("E2E: the colliding function may be declared before the `extern`") {
    CHECK(helpers::compile_and_run(R"(
        const write := fn(n: i32): i32 { return n + 1; };

        extern("ghoti_fake_libc", "write") const raw_write: fn(n: i32): i32;

        pub const main := fn(): i32 {
            return write(41);
        };
    )") == 42);
}

TEST_CASE("E2E: a `pub` function imported across modules still collides safely") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "sys.gh" as sys;

            pub const main := fn(): i32 {
                return sys::read(21);
            };
        )",
        {helpers::mock_file{"sys.gh", SYS_MODULE, "sys"}})};

    CHECK(exit_code == 42);
}

} // namespace ghoti::tests
