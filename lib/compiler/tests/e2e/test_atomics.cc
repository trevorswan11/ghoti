#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("@atomicStore then @atomicLoad observes the stored value") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut x: i32 = 0;
            @atomicStore(^mut x, 42, builtin.MemoryOrder.seq_cst);
            return @atomicLoad(i32, ^mut x, builtin.MemoryOrder.seq_cst);
        };
    )") == 42);
}

TEST_CASE("@atomicRmw(.add, ...) returns the old value and leaves the new one in memory") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut x: i32 = 10;
            let old = @atomicRmw(i32, ^mut x, builtin.AtomicRmwOp.add, 5,
                                    builtin.MemoryOrder.seq_cst);
            if (old != 10) { return 1; }
            if (x != 15) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@atomicRmw(.xchg, ...) swaps in the new value") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut x: i32 = 7;
            let old = @atomicRmw(i32, ^mut x, builtin.AtomicRmwOp.xchg, 99,
                                    builtin.MemoryOrder.seq_cst);
            if (old != 7) { return 1; }
            if (x != 99) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@cmpxchgStrong succeeds when 'expected' matches") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut x: i32 = 5;
            let mut out: i32 = 0;
            let ok = @cmpxchgStrong(i32, ^mut x, 5, 9, builtin.MemoryOrder.seq_cst,
                                       builtin.MemoryOrder.relaxed, ^mut out);
            if (!ok) { return 1; }
            if (x != 9) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@cmpxchgStrong fails and reports the actual value when 'expected' doesn't match") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut x: i32 = 5;
            let mut out: i32 = 0;
            let ok = @cmpxchgStrong(i32, ^mut x, 6, 9, builtin.MemoryOrder.seq_cst,
                                       builtin.MemoryOrder.relaxed, ^mut out);
            if (ok) { return 1; }
            if (x != 5) { return 2; }
            if (out != 5) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("@fence compiles and runs without effect on a single thread") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut x: i32 = 1;
            @fence(builtin.MemoryOrder.seq_cst);
            x = x + 1;
            @fence(builtin.MemoryOrder.acquire);
            return x;
        };
    )") == 2);
}

TEST_CASE("An untyped literal operand of an atomic takes the location's type") {
    CHECK(helpers::compile_and_run(R"(
        const Pair = extern struct { lo: u16, hi: u16 };
        pub const main = fn(): i32 {
            let mut wide: u64 = 0xFFFF_FFFF_FFFF_FFFF;
            @atomicStore(^mut wide, 3, builtin.MemoryOrder.seq_cst);
            let mut pair: Pair = .{ .lo = 0, .hi = 9 };
            @atomicStore(^mut pair.lo, 5, builtin.MemoryOrder.seq_cst);
            let mut x: u16 = 4;
            let widened: u64 = @atomicRmw(u16, ^mut x, builtin.AtomicRmwOp.add, 1,
                                         builtin.MemoryOrder.seq_cst);
            let mut out: u64 = 0;
            let swapped: bool = @cmpxchgStrong(u64, ^mut wide, 3, 7, builtin.MemoryOrder.seq_cst,
                                               builtin.MemoryOrder.seq_cst, ^mut out);
            if (!swapped) { return 1; }
            return @as(i32, @intCast(wide + pair.lo + pair.hi + widened + x));
        };
    )") == 7 + 5 + 9 + 4 + 5);
}

TEST_CASE("An atomic's order and op arguments accept an implicit variant") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut x: i32 = 10;
            @atomicStore(^mut x, 20, .release);
            let old = @atomicRmw(i32, ^mut x, .add, 5, .acq_rel);
            let mut out: i32 = 0;
            let swapped = @cmpxchgStrong(i32, ^mut x, 25, 40, .seq_cst, .relaxed, ^mut out);
            @fence(.seq_cst);
            if (old != 20) { return 1; }
            if (!swapped) { return 2; }
            return @atomicLoad(i32, ^mut x, .acquire);
        };
    )") == 40);
    // The variant must still exist, and keep the order rules
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            let mut x: i32 = 1;
            return @atomicRmw(i32, ^mut x, .bogus, 1, .seq_cst);
        };
    )");
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            let x: i32 = 1;
            return @atomicLoad(i32, ^x, .release);
        };
    )");
}

} // namespace ghoti::tests
