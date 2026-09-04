#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("@atomicStore then @atomicLoad observes the stored value") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 0;
            @atomicStore(^mut x, 42, builtin::MemoryOrder.seq_cst);
            return @atomicLoad(i32, ^mut x, builtin::MemoryOrder.seq_cst);
        };
    )") == 42);
}

TEST_CASE("@atomicRmw(.add, ...) returns the old value and leaves the new one in memory") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 10;
            const old := @atomicRmw(i32, ^mut x, builtin::AtomicRmwOp.add, 5,
                                    builtin::MemoryOrder.seq_cst);
            if (old != 10) { return 1; }
            if (x != 15) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@atomicRmw(.xchg, ...) swaps in the new value") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 7;
            const old := @atomicRmw(i32, ^mut x, builtin::AtomicRmwOp.xchg, 99,
                                    builtin::MemoryOrder.seq_cst);
            if (old != 7) { return 1; }
            if (x != 99) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@cmpxchgStrong succeeds when 'expected' matches") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            var out: i32 = 0;
            const ok := @cmpxchgStrong(i32, ^mut x, 5, 9, builtin::MemoryOrder.seq_cst,
                                       builtin::MemoryOrder.relaxed, ^mut out);
            if (!ok) { return 1; }
            if (x != 9) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@cmpxchgStrong fails and reports the actual value when 'expected' doesn't match") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            var out: i32 = 0;
            const ok := @cmpxchgStrong(i32, ^mut x, 6, 9, builtin::MemoryOrder.seq_cst,
                                       builtin::MemoryOrder.relaxed, ^mut out);
            if (ok) { return 1; }
            if (x != 5) { return 2; }
            if (out != 5) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@fence compiles and runs without effect on a single thread") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 1;
            @fence(builtin::MemoryOrder.seq_cst);
            x = x + 1;
            @fence(builtin::MemoryOrder.acquire);
            return x;
        };
    )") == 2);
}

} // namespace ghoti::tests
