#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/base.h>
#include <fmt/format.h>

#include "helpers/uir.hh"

namespace ghoti::tests {

namespace {

auto check_uir(std::string_view source, std::string_view expected) -> void {
    const auto actual{helpers::dump_uir(source)};
    if (actual != expected) { fmt::println("actual UIR:\n{}", actual); }
    CHECK(actual == expected);
}

} // namespace

TEST_CASE("UIR: locals are values, places, or comptime bodies") {
    check_uir(R"(const f = fn(a: i32): i32 {
    let b = a + 1;
    let mut c: i32 = b;
    c += 2;
    const d = 3;
    return c * d;
};)",
              R"(decl#0 VALUE f CONSTANT
    value {
        %18 = func conv=C
            params {
                %1 = param name="a", modifier=VALUE
                    type {
                        yield @i32_type
                    }
            }
            ret {
                yield @i32_type
            }
            body {
                %3 = int_value 1
                %4 = add %1, %3
                %5 = let_value name="b", init=%4
                %6 = alloc_mut name="c", type=@i32_type
                store %6, %5
                %8 = load %6
                %9 = int_value 2
                %10 = add %8, %9
                store %6, %10
                %14 = const_local name="d"
                    value {
                        %12 = int_value 3
                        yield %12
                    }
                %15 = load %6
                %16 = mul %15, %14
                ret %16
            }
        yield %18
    }
)");
}

TEST_CASE("UIR: a labeled loop is the target of break and continue") {
    check_uir(R"(const f = fn(n: i32): i32 {
    let mut i: i32 = 0;
    outer: while (i < n) : (i += 1) {
        if (i == 3) { break :outer; }
        continue :outer;
    }
    return i;
};)",
              R"(decl#0 VALUE f CONSTANT
    value {
        %25 = func conv=C
            params {
                %1 = param name="n", modifier=VALUE
                    type {
                        yield @i32_type
                    }
            }
            ret {
                yield @i32_type
            }
            body {
                %3 = alloc_mut name="i", type=@i32_type
                %4 = int_value 0
                store %3, %4
                %9 = loop label="outer", flags=has_cond
                    cond {
                        %6 = load %3
                        %7 = cmp_lt %6, %1
                        yield %7
                    }
                    body {
                        %14 = load %3
                        %15 = int_value 3
                        %16 = cmp_eq %14, %15
                        %21 = condbr cond=%16
                            then {
                                %18 = block
                                    body {
                                        break_to %9, @void_value
                                    }
                                yield @void_value
                            }
                            else {
                                yield @void_value
                            }
                        repeat %9
                    }
                    cont {
                        %10 = load %3
                        %11 = int_value 1
                        %12 = add %10, %11
                        store %3, %12
                    }
                %23 = load %3
                ret %23
            }
        yield %25
    }
)");
}

TEST_CASE("UIR: every exit runs a copy of the defers it owes") {
    check_uir(R"(const a = fn(): void {};
const b = fn(): void {};
const c = fn(): bool { return true; };
const f = fn(): void {
    defer a();
    {
        defer b();
        if (c()) { return; }
    }
    loop {
        defer b();
        break;
    }
};)",
              R"(decl#0 VALUE a CONSTANT
    value {
        %1 = func conv=C
            ret {
                yield @void_type
            }
        yield %1
    }
decl#1 VALUE b CONSTANT
    value {
        %4 = func conv=C
            ret {
                yield @void_type
            }
        yield %4
    }
decl#2 VALUE c CONSTANT
    value {
        %8 = func conv=C
            ret {
                yield @bool_type
            }
            body {
                ret @bool_true
            }
        yield %8
    }
decl#3 VALUE f CONSTANT
    value {
        %31 = func conv=C
            ret {
                yield @void_type
            }
            body {
                %24 = block
                    body {
                        %11 = decl_val decl#2
                        %12 = call callee=%11
                        %21 = condbr cond=%12
                            then {
                                %18 = block
                                    body {
                                        %13 = decl_val decl#1
                                        %14 = call callee=%13
                                        %15 = decl_val decl#0
                                        %16 = call callee=%15
                                        ret @void_value
                                    }
                                yield @void_value
                            }
                            else {
                                yield @void_value
                            }
                        %22 = decl_val decl#1
                        %23 = call callee=%22
                    }
                %25 = loop
                    body {
                        %26 = decl_val decl#1
                        %27 = call callee=%26
                        break_to %25, @void_value
                    }
                %29 = decl_val decl#0
                %30 = call callee=%29
            }
        yield %31
    }
)");
}

TEST_CASE("UIR: errdefer runs on a ? propagation and on a failing return") {
    check_uir(R"(const log = fn(e: u8): void {};
const f = fn(x: u8): u8 {
    errdefer |e| log(e);
    const v = x?;
    return v;
};)",
              R"(decl#0 VALUE log CONSTANT
    value {
        %3 = func conv=C
            params {
                %1 = param name="e", modifier=VALUE
                    type {
                        yield @u8_type
                    }
            }
            ret {
                yield @void_type
            }
        yield %3
    }
decl#1 VALUE f CONSTANT
    value {
        %23 = func conv=C
            params {
                %6 = param name="x", flags=comptime, modifier=VALUE
                    type {
                        yield @u8_type
                    }
            }
            ret {
                yield @u8_type
            }
            body {
                %14 = const_local name="v"
                    value {
                        %12 = unwrap operand=%6, kind=PROPAGATE
                            failure {
                                %8 = residual
                                %9 = decl_val decl#0
                                %10 = call callee=%9, args=(%8)
                                ret_residual %8
                            }
                        yield %12
                    }
                %15 = ret_failure %14
                %16 = failure_is_set %15
                %22 = condbr cond=%16
                    then {
                        %17 = failure_residual %15
                        %18 = decl_val decl#0
                        %19 = call callee=%18, args=(%17)
                        ret %14
                    }
                    else {
                        ret %14
                    }
            }
        yield %23
    }
)");
}

TEST_CASE("UIR: a closure captures what it writes by reference, the rest by value") {
    check_uir(R"(const f = fn(): i32 {
    let mut counted: i32 = 0;
    let seen: i32 = 1;
    let bump = fn(): void { counted += seen; };
    let copy = move fn(): i32 { counted = 5; return counted; };
    bump();
    return copy() + counted;
};)",
              R"(decl#0 VALUE f CONSTANT
    value {
        %30 = func conv=C
            ret {
                yield @i32_type
            }
            body {
                %1 = alloc_mut name="counted", type=@i32_type
                %2 = int_value 0
                store %1, %2
                %4 = int_value 1
                %5 = let_value name="seen", type=@i32_type, init=%4
                %13 = func conv=C
                    ret {
                        yield @void_type
                    }
                    body {
                        %8 = capture capture#0
                        %9 = load %8
                        %10 = capture capture#1
                        %11 = add %9, %10
                        store %8, %11
                    }
                %14 = closure func=%13, captures=[value=%1, by_ref=1, name="counted"; value=%5, by_ref=0, name="seen"]
                %6 = let_value name="bump", init=%14
                %22 = func conv=C, flags=move
                    ret {
                        yield @i32_type
                    }
                    body {
                        %17 = capture capture#0
                        %18 = int_value 5
                        store %17, %18
                        %20 = capture capture#0
                        ret %20
                    }
                %23 = load %1
                %24 = closure func=%22, captures=[value=%23, by_ref=0, name="counted"]
                %15 = let_value name="copy", init=%24
                %25 = call callee=%6
                %26 = call callee=%15
                %27 = load %1
                %28 = add %26, %27
                ret %28
            }
        yield %30
    }
)");
}

TEST_CASE("UIR: a type constructor's members capture its comptime parameter") {
    check_uir(R"(const Box = fn(comptime T: type): type {
    return struct {
        v: T,
        pub const get = fn(self): T { return self.v; };
    };
};)",
              R"(decl#0 VALUE Box CONSTANT
    value {
        %16 = func conv=C
            params {
                %1 = param name="T", flags=comptime|comptime_written, modifier=VALUE
                    type {
                        yield @type_type
                    }
            }
            ret {
                yield @type_type
            }
            body {
                %3 = struct_decl captures=[value=%1, by_ref=0, name="T"], fields=[name="v"], decls=[decl#1]
                    fields[0].type {
                        %13 = capture capture#0
                        yield %13
                    }
                ret %3
            }
        yield %16
    }
decl#1 VALUE get PUBLIC CONSTANT in %3
    value {
        %9 = func conv=C
            params {
                %4 = param name="self", flags=self, modifier=VALUE
            }
            ret {
                %5 = capture capture#0
                yield %5
            }
            body {
                %7 = field_val object=%4, name="v"
                ret %7
            }
        %10 = capture capture#0
        %11 = closure func=%9, captures=[value=%10, by_ref=0, name="T"]
        yield %11
    }
)");
}

TEST_CASE("UIR: match arms bind their captures once for every alternative") {
    check_uir(R"(const Shape = union(enum) { circle: i32, square: i32 };
const area = fn(s: Shape): i32 {
    return match (s) {
        .circle(|r|), .square(|r|) if r > 0 => r * r,
        _ => 0,
    };
};)",
              R"(decl#0 VALUE Shape CONSTANT
    value {
        %0 = union_decl tag=INFERRED, fields=[name="circle"; name="square"]
            fields[0].type {
                yield @i32_type
            }
            fields[1].type {
                yield @i32_type
            }
        yield %0
    }
decl#1 VALUE area CONSTANT
    value {
        %27 = func conv=C
            params {
                %6 = param name="s", modifier=VALUE
                    type {
                        %4 = decl_val decl#0
                        yield %4
                    }
            }
            ret {
                yield @i32_type
            }
            body {
                %25 = match operand=%6, catch_all=1, arms=[patterns=(%11, %15); patterns=(%22)]
                    arms[0].pattern_body {
                        %10 = pat_capture name="r", slot=0, modifier=VALUE
                        %11 = pat_call args=(%10)
                            callee {
                                %8 = implicit_access "circle"
                                yield %8
                            }
                        %14 = pat_capture name="r", slot=0, modifier=VALUE
                        %15 = pat_call args=(%14)
                            callee {
                                %12 = implicit_access "square"
                                yield %12
                            }
                        %16 = bind name="r", slot=0, modifier=VALUE
                    }
                    arms[0].guard {
                        %17 = int_value 0
                        %18 = cmp_gt %16, %17
                        yield %18
                    }
                    arms[0].body {
                        %20 = mul %16, %16
                        yield %20
                    }
                    arms[1].pattern_body {
                        %22 = pat_wildcard
                    }
                    arms[1].body {
                        %23 = int_value 0
                        yield %23
                    }
                ret %25
            }
        yield %27
    }
)");
}

TEST_CASE("UIR: an unwrap capture splits into a payload and a residual body") {
    check_uir(R"(const f = fn(x: u8): u8 {
    return if (x) |v| v else |e| 0;
};)",
              R"(decl#0 VALUE f CONSTANT
    value {
        %10 = func conv=C
            params {
                %1 = param name="x", modifier=VALUE
                    type {
                        yield @u8_type
                    }
            }
            ret {
                yield @u8_type
            }
            body {
                %8 = unwrap_branch operand=%1
                    then {
                        %3 = bind name="v", slot=0, modifier=VALUE
                        yield %3
                    }
                    else {
                        %5 = bind name="e", slot=1, modifier=VALUE
                        %6 = int_value 0
                        yield %6
                    }
                ret %8
            }
        yield %10
    }
)");
}

TEST_CASE("UIR: a bodyless function type is a func flagged type_expr") {
    check_uir("const n = @typeName(fn(x: i32): bool);", R"(decl#0 VALUE n CONSTANT
    value {
        %3 = func conv=C, flags=type_expr
            params {
                %1 = param name="x", modifier=VALUE
                    type {
                        yield @i32_type
                    }
            }
            ret {
                yield @bool_type
            }
        %4 = builtin builtin=@typeName, args=(%3)
        yield %4
    }
)");
}

} // namespace ghoti::tests
