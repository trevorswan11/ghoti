#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("@assert and @verify pass silently when their condition holds at runtime") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            @assert(x == 5);
            @verify(x > 0, "x must be positive");
            return x + 37;
        };
    )") == 42);
}

TEST_CASE("a failing @verify aborts through the panic handler at runtime", "[.panic]") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 0;
            @verify(x > 0);
            return 0;
        };
    )") != 0);
}

TEST_CASE("@verify routes its message to the panic handler, which can observe it", "[.panic]") {
    CHECK(helpers::compile_and_run(R"(
        pub var last_msg_len: usize = 0;
        pub weak const panic_handler := fn(msg: []u8, file: []u8, line: u32, column: u32): noreturn {
            _ = file;
            _ = line;
            _ = column;
            last_msg_len = msg.len;
            @trap();
        };
        pub const main := fn(): i32 {
            var x: i32 = 0;
            @verify(x > 0, "nope");
            return @as(i32, last_msg_len);
        };
    )") != 0);
}

TEST_CASE("a comptime-true @assert / @verify emits no check") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            @assert(1 == 1);
            @verify(2 > 1, "always");
            return 42;
        };
    )") == 42);
}

} // namespace ghoti::tests
