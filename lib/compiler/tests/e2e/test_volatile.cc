#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Volatile enum variable with implicit member access compiles and runs (issue 286)") {
    CHECK(helpers::compile_and_run(R"(
        const State := enum : i32 {
            INIT_PULL_LINE_LOW,
            INPUT_JUST_ENABLED,
            HIGH_ACK,
            BIT_READ_RISING,
            READ_COMPLETE,
            ERROR_STATE
        };

        var currentState: volatile State = .READ_COMPLETE;

        pub const main := fn(): i32 {
            if (currentState == .READ_COMPLETE) {
                currentState = .HIGH_ACK;
                if (currentState == .HIGH_ACK) {
                    return 0;
                }
            }
            return 1;
        };
    )") == 0);
}

TEST_CASE("Volatile local enum variable with implicit member access compiles and runs") {
    CHECK(helpers::compile_and_run(R"(
        const State := enum : i32 {
            A,
            B,
            C
        };

        pub const main := fn(): i32 {
            var s: mut volatile State = .B;
            if (s != .B) { return 1; }
            s = .C;
            if (s != .C) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("Volatile global variable with constant initializer compiles and runs (issue 287)") {
    CHECK(helpers::compile_and_run(R"(
        var current_reading_bit_idx: volatile i32 = 0;
        var neg_val: volatile i32 = -100;
        var flag: volatile bool = true;
        var expr_val: volatile i32 = 10 + 20 * 2;

        pub const main := fn(): i32 {
            if (current_reading_bit_idx != 0) { return 1; }
            if (neg_val != -100) { return 2; }
            if (!flag) { return 3; }
            if (expr_val != 50) { return 4; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("Volatile local and global variable loads and stores operate correctly at runtime") {
    CHECK(helpers::compile_and_run(R"(
        var g_vol: mut volatile i32 = 10;

        pub const main := fn(): i32 {
            var loc_vol: mut volatile i32 = 20;
            g_vol = g_vol + 5;
            loc_vol = loc_vol + 10;
            g_vol += 2;
            loc_vol *= 2;
            if (g_vol != 17) { return 1; }
            if (loc_vol != 60) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("Volatile pointer load and store operate correctly at runtime") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: mut volatile i32 = 42;
            const p: ^mut volatile i32 = ^mut x;
            *p = 99;
            if (x != 99) { return 1; }
            if (*p != 99) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("Volatile struct field access and mutation operate correctly at runtime") {
    CHECK(helpers::compile_and_run(R"(
        const DeviceRegisters := struct {
            status: i32,
            control: i32,
            data: i32,
        };

        var regs: mut volatile DeviceRegisters = DeviceRegisters{
            .status = 1,
            .control = 2,
            .data = 3,
        };

        pub const main := fn(): i32 {
            if (regs.status != 1) { return 1; }
            if (regs.control != 2) { return 1; }
            if (regs.data != 3) { return 1; }
            regs.data = 42;
            regs.control = 10;
            if (regs.data != 42) { return 2; }
            if (regs.control != 10) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("Volatile array indexing and mutation operate correctly at runtime") {
    CHECK(helpers::compile_and_run(R"(
        var buffer: mut volatile [4uz]mut i32 = [4uz]mut i32{10, 20, 30, 40};

        pub const main := fn(): i32 {
            if (buffer[0uz] != 10) { return 1; }
            if (buffer[1uz] != 20) { return 1; }
            if (buffer[2uz] != 30) { return 1; }
            if (buffer[3uz] != 40) { return 1; }
            buffer[2uz] = 99;
            if (buffer[2uz] != 99) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("Type inference on volatile variable does not infer volatile type") {
    CHECK(helpers::compile_and_run(R"(
        var vol_val: volatile i32 = 10;

        pub const main := fn(): i32 {
            var copy := vol_val;
            copy = copy + 5;
            if (copy != 15) { return 1; }
            if (vol_val != 10) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("Reading volatile variable is rejected in const eval") {
    helpers::expect_compile_error(R"(
        var vol_val: volatile i32 = 10;
        const bad_const: i32 = vol_val;
        pub const main := fn(): void {};
    )");
}

} // namespace ghoti::tests
