#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

struct named_type {
    std::string_view name;
    sema::type&      type;
};

// `body` as the statements of a `main` that sees a few globals of different types
[[nodiscard]] auto with_globals(std::string_view body) -> std::string {
    return fmt::format(R"(
        let mut flag: bool = true;
        let mut small: i8 = -5;
        let mut byte: u8 = 200;
        let mut word: i32 = 9;
        let mut uword: u32 = 4000000000;
        let mut wide: i64 = 1000000000000;
        let mut single: f32 = 1.5;
        let mut real: f64 = 0.5;
        let mut three: [3]i32 = .{{ 1, 2, 3 }};
        let mut five: [5]mut i32 = .{{ 10, 20, 30, 40, 50 }};
        pub const main = fn(): i32 {{
            {}
            return 0;
        }};
    )",
                       body);
}

[[nodiscard]] auto runs(std::string_view body) -> u32 {
    return helpers::compile_and_run(with_globals(body));
}

[[nodiscard]] auto no_peer(std::string_view body) -> helpers::resolve_result {
    return helpers::resolve_diags(with_globals(body));
}

} // namespace

TEST_CASE("@TypeOf of two numbers is the type the lattice says both widen into") {
    auto             ctx_idx{helpers::type_check("")};
    auto&            ctx{*ctx_idx.first};
    const std::array types{
        named_type{"i8", ctx.get_int_type(8, true)},
        named_type{"u8", ctx.get_int_type(8, false)},
        named_type{"i16", ctx.get_int_type(16, true)},
        named_type{"u16", ctx.get_int_type(16, false)},
        named_type{"i32", ctx.get_int_type(32, true)},
        named_type{"u32", ctx.get_int_type(32, false)},
        named_type{"i64", ctx.get_int_type(64, true)},
        named_type{"u64", ctx.get_int_type(64, false)},
        named_type{"i128", ctx.get_int_type(128, true)},
        named_type{"u128", ctx.get_int_type(128, false)},
        named_type{"isize", ctx.get_type(sema::type_kind::ISIZE)},
        named_type{"usize", ctx.get_type(sema::type_kind::USIZE)},
        named_type{"f16", ctx.get_type(sema::type_kind::F16)},
        named_type{"f32", ctx.get_type(sema::type_kind::F32)},
        named_type{"f64", ctx.get_type(sema::type_kind::F64)},
        named_type{"f128", ctx.get_type(sema::type_kind::F128)},
    };

    for (const auto& lhs : types) {
        for (const auto& rhs : types) {
            // No type is invented: the peer is whichever operand the other widens into
            std::string_view expected;
            if (lhs.name == rhs.name || sema::is_implicit_widenable(rhs.type, lhs.type)) {
                expected = lhs.name;
            } else if (sema::is_implicit_widenable(lhs.type, rhs.type)) {
                expected = rhs.name;
            }

            const auto source{fmt::format(R"(
                let mut a: {0} = undefined;
                let mut b: {1} = undefined;
                const same = @TypeOf(a, b) == {2};
                const checked = if comptime (same) 1 else @compileError("wrong peer");
            )",
                                          lhs.name,
                                          rhs.name,
                                          expected.empty() ? "void" : expected)};
            const auto result{helpers::resolve_diags(source)};
            INFO(lhs.name << " with " << rhs.name << " expects "
                          << (expected.empty() ? "no peer" : expected));
            if (expected.empty()) {
                CHECK(std::ranges::contains(result.codes, sema::error::NO_PEER_TYPE));
            } else {
                CHECK(result.codes.empty());
            }
        }
    }
}

TEST_CASE("mixed-width arithmetic converts both operands to their peer at runtime") {
    CHECK(runs(R"(
        let a = small + wide;        // sign-extends a negative i8
        if (@TypeOf(a) != i64) { return 1; }
        if (a != 999999999995) { return 2; }

        let b = uword + wide;        // zero-extends a u32 above the i32 range
        if (b != 1004000000000) { return 3; }

        let huge: i128 = wide;
        if (huge * small != -5000000000000) { return 4; }
        let from_unsigned: i128 = @as(u64, 18446744073709551615);
        if (from_unsigned + byte != 18446744073709551815) { return 5; }

        let c = word * real;         // an i32 is exact in an f64
        if (@TypeOf(c) != f64) { return 6; }
        if (c != 4.5) { return 7; }
        let d = single + real;
        if (@TypeOf(d) != f64 or d != 2.0) { return 8; }

        if (!(small < wide)) { return 9; }
        if (byte == wide) { return 10; }
        if (small >= word) { return 11; }
    )") == 0);
}

TEST_CASE("mixed-width arithmetic folds to the value it computes") {
    CHECK(runs(R"(
        let a: i64 = @as(i32, 7) + @as(i64, 5000000000);
        if (a != 5000000007) { return 1; }
        let b = @as(i8, -5) * @as(i128, 1000000000000);
        if (@TypeOf(b) != i128 or b != -5000000000000) { return 2; }
        let c = @as(u8, 200) + @as(i16, 100);
        if (@TypeOf(c) != i16 or c != 300) { return 3; }
        let d = @as(i32, 3) * @as(f64, 0.5);
        if (@TypeOf(d) != f64 or d != 1.5) { return 4; }
        let e = @as(u16, 65535) < @as(i32, -1);
        if (e) { return 5; }
        let f = @max(@as(i8, -1), @as(i64, 5000000000));
        if (@TypeOf(f) != i64 or f != 5000000000) { return 6; }
    )") == 0);
}

TEST_CASE("an untyped constant takes the concrete operand's type, range-checked") {
    CHECK(runs(R"(
        let a = byte + 1;
        if (@TypeOf(a) != u8 or a != 201) { return 1; }
        let b = 2.5 * single;
        if (@TypeOf(b) != f32) { return 2; }
        let c = if (flag) wide else 7;
        if (@TypeOf(c) != i64 or c != 1000000000000) { return 3; }
    )") == 0);
}

TEST_CASE("the arms of an if meet at their peer type") {
    CHECK(runs(R"(
        let a = if (flag) small else wide;
        if (@TypeOf(a) != i64 or a != -5) { return 1; }
        let b = if (!flag) small else wide;
        if (b != 1000000000000) { return 2; }
        let c = if (flag) byte else real;
        if (@TypeOf(c) != f64 or c != 200.0) { return 3; }

        // An arm that leaves takes no part
        let d = if (flag) word else return 4;
        if (@TypeOf(d) != i32 or d != 9) { return 5; }
        let e = if (!flag) return 6 else wide;
        if (@TypeOf(e) != i64) { return 7; }
        let f = if (flag) word else undefined;
        if (@TypeOf(f) != i32 or f != 9) { return 8; }
    )") == 0);
}

TEST_CASE("the arms of a match meet at their peer type") {
    CHECK(runs(R"(
        let a = match (byte) {
            200 => small,
            1 => wide,
            _ => 7,
        };
        if (@TypeOf(a) != i64 or a != -5) { return 1; }

        let b = match (byte) {
            1 => return 2,
            200 => word,
            _ => wide,
        };
        if (@TypeOf(b) != i64 or b != 9) { return 3; }
    )") == 0);
}

TEST_CASE("the values a label is broken with meet at their peer type") {
    CHECK(runs(R"(
        let a = blk: {
            if (flag) { break :blk byte; }
            break :blk wide;
        };
        if (@TypeOf(a) != i64 or a != 200) { return 1; }

        let mut i: i32 = 0;
        let b = outer: while (i < 10) : (i += 1) {
            if (i == 3) { break :outer small; }
            if (i == 8) { break :outer wide; }
        } else wide;
        if (@TypeOf(b) != i64 or b != -5) { return 2; }
    )") == 0);
}

TEST_CASE("an if or match nothing reads needs no common type") {
    CHECK(runs(R"(
        if (flag) word = 1 else flag = true;
        match (byte) {
            1 => word = 2,
            _ => flag = false,
        }
        if (word != 1 or flag) { return 1; }
    )") == 0);
}

TEST_CASE("pointers meet at the least mutable, and nullptr takes the pointer's type") {
    CHECK(runs(R"(
        let mut other: i32 = 4;
        let constant: ^i32 = ^word;
        let mutable: ^mut i32 = ^mut other;
        let a = if (flag) mutable else constant;
        if (@TypeOf(a) != ^i32 or *a != 4) { return 1; }
        let b = if (!flag) mutable else nullptr;
        if (@TypeOf(b) != ^mut i32 or b != nullptr) { return 2; }
        let c = if (flag) nullptr else constant;
        if (@TypeOf(c) != ^i32 or c != nullptr) { return 3; }
        if (@TypeOf(mutable, constant, nullptr) != ^i32) { return 4; }

        let view: []i32 = three;
        let writable: []mut i32 = five;
        let d = if (flag) writable else view;
        if (@TypeOf(d) != []i32 or d.len != 5) { return 5; }
    )") == 0);
}

TEST_CASE("arrays of different lengths meet as a slice") {
    CHECK(runs(R"(
        let a = if (flag) three else five;
        if (@TypeOf(a) != []i32) { return 1; }
        if (a.len != 3 or a[2] != 3) { return 2; }
        let b = if (!flag) three else five;
        if (b.len != 5 or b[4] != 50) { return 3; }

        let same = if (flag) three else three;
        if (@TypeOf(same) != [3]i32) { return 4; }
        let mixed = if (flag) &three else &five;
        if (@TypeOf(mixed) != []i32 or mixed.len != 3 or mixed[1] != 2) { return 5; }
        let other = if (!flag) &three else &five;
        if (other.len != 5 or other[4] != 50) { return 6; }
    )") == 0);
}

TEST_CASE("pointers to arrays of different lengths have no peer") {
    CHECK(helpers::raised(R"(
        pub const main = fn(): i32 {
            let mut three: [3]i32 = .{ 1, 2, 3 };
            let mut five: [5]i32 = .{ 1, 2, 3, 4, 5 };
            let mixed = if (three[0] == 1) ^three else ^five;
            _ = mixed;
            return 0;
        };
    )",
                          sema::error::NO_PEER_TYPE));
}

TEST_CASE("a slice of two array temporaries stays valid after the expression") {
    CHECK(helpers::compile_and_run(R"(
        let mut flag: bool = true;
        const three = fn(): [3]i32 { return .{ 1, 2, 3 }; };
        const five = fn(): [5]i32 { return .{ 10, 20, 30, 40, 50 }; };
        const clobber = fn(): i32 {
            let mut junk: [64]mut i32 = undefined;
            let mut i: usize = 0;
            while (i < 64) : (i += 1) { junk[i] = -1; }
            return junk[3];
        };

        pub const main = fn(): i32 {
            let a = if (flag) three() else five();
            let b = if (!flag) three() else five();
            if (clobber() != -1) { return 1; }
            if (a.len != 3 or a[0] != 1 or a[2] != 3) { return 2; }
            if (b.len != 5 or b[4] != 50) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("a local declared in a loop body reuses one stack slot") {
    // 200000 iterations of a 4 KiB temporary would need 800 MiB of stack otherwise
    CHECK(helpers::compile_and_run(R"(
        let mut flag: bool = true;
        const big = fn(): [4096]u8 { let mut a: [4096]u8 = undefined; return a; };
        const small = fn(): [16]u8 { let mut a: [16]u8 = undefined; return a; };

        pub const main = fn(): i32 {
            let mut total: usize = 0;
            let mut i: usize = 0;
            while (i < 200000) : (i += 1) {
                let local = big();
                let view = if (flag) big() else small();
                total += view.len + local.len;
            }
            if (total != 200000 * 8192) { return 1; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("builtins with two operands convert them to their peer") {
    CHECK(runs(R"(
        if (@max(small, wide) != 1000000000000) { return 1; }
        if (@TypeOf(@min(byte, wide)) != i64 or @min(byte, wide) != 200) { return 2; }
        if (@min(small, real) != -5.0) { return 3; }
        if (@TypeOf(@divTrunc(wide, byte)) != i64) { return 4; }
        if (@divFloor(small, wide) != -1) { return 5; }
        if (@rem(wide, byte) != 0 or @mod(small, word) != 4) { return 6; }
        if (@TypeOf(@min(word, 7) + @max(3, wide)) != i64) { return 7; }

        let mut sum: i64 = 0;
        if (@addWithOverflow(small, wide, &mut sum)) { return 8; }
        if (sum != 999999999995) { return 9; }
        let mut product: i64 = 0;
        if (!@mulWithOverflow(wide, wide, &mut product)) { return 10; }
        let mut narrow: u8 = 0;
        if (!@addWithOverflow(byte, 100, &mut narrow)) { return 11; }
        if (narrow != 44) { return 12; }
    )") == 0);

    const auto slot{no_peer(R"(
        let mut out: i32 = 0;
        const overflowed = @addWithOverflow(word, wide, &mut out);
    )")};
    CHECK(slot.message_contains("expects its third argument to be a '&mut i64' result reference"));
}

TEST_CASE("@TypeOf takes any number of operands") {
    CHECK(runs(R"(
        if (@TypeOf(small, byte, wide) != i64) { return 1; }
        if (@TypeOf(byte, single) != f32) { return 2; }
        if (@TypeOf(1, 2.5) != @TypeOf(2.5)) { return 3; }
        if (@TypeOf(word) != i32) { return 4; }

        // In a type position
        let mut a: @TypeOf(small, wide) = small;
        a += 1;
        if (a != -4) { return 5; }

        // In a comptime condition
        let b = if comptime (@TypeOf(byte, word) == i32) 1 else 2;
        if (b != 1) { return 6; }

        // Its operands are never evaluated
        const T = @TypeOf(word, wide / (wide - wide));
        if (T != i64) { return 7; }
    )") == 0);

    CHECK(helpers::raised("const T = @TypeOf();", sema::error::ARITY_MISMATCH));
}

TEST_CASE("a generic's peer types follow each instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const add = fn(a: auto, b: auto): @TypeOf(a, b) {
            let sum = a + b;
            return if (a < b) sum else sum;
        };
        const Peer = fn(A: type, B: type): type {
            let mut a: A = undefined;
            let mut b: B = undefined;
            return @TypeOf(a, b);
        };

        pub const main = fn(): i32 {
            let small: i32 = 1;
            let big: i64 = 5000000000;
            if (@TypeOf(add(small, big)) != i64) { return 1; }
            if (add(small, big) != 5000000001) { return 2; }
            if (@TypeOf(add(big, big)) != i64 or add(big, big) != 10000000000) { return 3; }
            if (@TypeOf(add(small, small)) != i32 or add(small, small) != 2) { return 4; }
            if (add(big, small) != 5000000001) { return 5; }

            if (Peer(u8, i32) != i32) { return 6; }
            if (Peer(f32, f64) != f64) { return 7; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("operands with no peer type are rejected where they meet") {
    SECTION("binary operators") {
        const auto mixed_sign{no_peer("const a = word + uword;")};
        CHECK(std::ranges::contains(mixed_sign.codes, sema::error::NO_PEER_TYPE));
        CHECK(mixed_sign.message_contains(
            "no peer type for 'i32' and 'u32'; convert one with `@intCast`"));

        const auto inexact{no_peer("const a = wide + real;")};
        CHECK(inexact.message_contains(
            "no peer type for 'i64' and 'f64'; convert the integer with `@floatFromInt`"));

        CHECK(std::ranges::contains(no_peer("const a = word < uword;").codes,
                                    sema::error::NO_PEER_TYPE));
        CHECK(std::ranges::contains(no_peer("const a = word + 2.5;").codes,
                                    sema::error::NO_PEER_TYPE));
    }

    SECTION("if, match, and break") {
        CHECK(no_peer("const a = if (flag) word else flag;")
                  .message_contains("no peer type for 'i32' and 'bool'"));
        CHECK(no_peer("const a = match (byte) { 1 => word, _ => uword, };")
                  .message_contains("no peer type for 'i32' and 'u32'"));
        CHECK(no_peer("const a = blk: { if (flag) { break :blk word; } break :blk uword; };")
                  .message_contains("no peer type for 'i32' and 'u32'"));
    }

    SECTION("arrays of different elements") {
        CHECK(no_peer(R"(
            let mut narrow: [3]u8 = .{ 1, 2, 3 };
            let mut broad: [4]u16 = .{ 1, 2, 3, 4 };
            const a = if (flag) narrow else broad;
        )")
                  .message_contains("no peer type for '[3]u8' and '[4]u16'"));
    }

    SECTION("builtins") {
        CHECK(no_peer("const a = @max(word, uword);")
                  .message_contains("'@max': no peer type for 'i32' and 'u32'"));
        CHECK(no_peer("const T = @TypeOf(word, flag);")
                  .message_contains("'@TypeOf': no peer type for 'i32' and 'bool'"));
    }

    SECTION("a shift keeps its left operand's type") {
        CHECK_FALSE(std::ranges::contains(no_peer("const a = word << wide;").codes,
                                          sema::error::NO_PEER_TYPE));
    }
}

} // namespace ghoti::tests
