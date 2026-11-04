#include <string>
#include <string_view>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fmt/format.h>

#include <stdx/arena.hh>

#include "compiler/ast/ast.hh"
#include "compiler/sema/error.hh"
#include "compiler/syntax/parser.hh"

#include "helpers/codegen.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view SHAPES{R"(
    const O = union(enum) { some: i32, none: void };
    const R = union(enum) { ok: O, err: i32 };
    const P = struct { pub x: i32, pub y: i32 };
    const F = packed struct { pub a: u3, pub b: u5 };
    const r = R{ .err = 0 };
    const p = P{ .x = 0, .y = 0 };
    const bump = fn(n: &mut i32): bool { n = n + 1; return true; };
    const assign = fn(n: &mut i32): bool { n = 1; return true; };
)"};

[[nodiscard]] auto rejects(std::string_view body, sema::error code) -> bool {
    return helpers::raised(fmt::format("{}\npub const main = fn(): i32 {{ {} }};", SHAPES, body),
                           code);
}

} // namespace

TEST_CASE("Slices match their length and strings their bytes") {
    SECTION("a slice's length, binding the whole slice") {
        CHECK(helpers::compile_and_run(R"(
            const O = union(enum) { some: []u8, none: void };
            const R = union(enum) { ok: O, err: i32 };
            const f = fn(r: R): usize {
                return match (r) {
                    .ok(.some(.{ .len = 1.. } |raw|)) => raw.len,
                    _ => 0,
                };
            };
            pub const main = fn(): i32 {
                let mut buf = [3]u8{ 1, 2, 3 };
                const empty = R{ .ok = O{ .some = buf[0..0] } };
                const full = R{ .ok = O{ .some = buf[0..3] } };
                return @intCast(i32, f(empty) + f(full));
            };
        )") == 3);
    }

    SECTION("string literals, including one sharing a prefix") {
        CHECK(helpers::compile_and_run(R"(
            const f = fn(s: []u8): i32 {
                return match (s) {
                    "" => 1,
                    "main" => 2,
                    "mail" => 3,
                    "m" => 4,
                    .{ .len = 4 } => 5,
                    _ => 6,
                };
            };
            pub const main = fn(): i32 {
                const ok = f("") == 1 and f("main") == 2 and f("mail") == 3 and f("m") == 4 and f("mint") == 5 and f("nope!") == 6;
                return if (ok) 1 else 0;
            };
        )") == 1);
    }

    SECTION("a string nested in a struct") {
        CHECK(helpers::compile_and_run(R"(
            const N = struct { pub name: []u8, pub n: i32 };
            const f = fn(v: N): i32 {
                return match (v) {
                    .{ .name = "add", .n = |n| } => n + 1,
                    .{ .name = "sub", .n = |n| } => n - 1,
                    _ => 0,
                };
            };
            pub const main = fn(): i32 {
                return f(N{ .name = "add", .n = 10 }) * 2 + f(N{ .name = "sub", .n = 10 }) + f(N{ .name = "x", .n = 3 });
            };
        )") == 31);
    }

    SECTION("a byte array") {
        CHECK(helpers::compile_and_run(R"(
            const f = fn(a: [2]u8): i32 {
                return match (a) { "ok" => 1, _ => 2 };
            };
            pub const main = fn(): i32 {
                return f([2]u8{ 'o', 'k' }) * 10 + f([2]u8{ 'n', 'o' });
            };
        )") == 12);
    }

    SECTION("folded at compile time") {
        CHECK(helpers::compile_and_run(R"(
            const f = fn(s: []u8): i32 {
                return match (s) {
                    "main" => 2,
                    .{ .len = 3 } => 3,
                    _ => 4,
                };
            };
            const A: i32 = f("main");
            const B: i32 = f("abc");
            const C: i32 = f("z");
            pub const main = fn(): i32 { return A * 100 + B * 10 + C; };
        )") == 234);
    }

    SECTION("a string can't overrun a shorter value") {
        CHECK(helpers::compile_and_run(R"(
            const f = fn(s: []u8): i32 { return match (s) { "abcdef" => 1, _ => 2 }; };
            pub const main = fn(): i32 {
                let mut one = [1]u8{ 'a' };
                return f(one[0..1]);
            };
        )") == 2);
    }
}

TEST_CASE("Local declarations destructure a value") {
    SECTION("struct fields") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32, pub y: i32 };
            pub const main = fn(): i32 {
                let p = P{ .x = 3, .y = 4 };
                let .{ .x = |x|, .y = |y| } = p;
                return x * y;
            };
        )") == 12);
    }

    SECTION("nested, with omitted fields and discards") {
        CHECK(helpers::compile_and_run(R"(
            const I = struct { pub a: i32, pub b: i32 };
            const O = struct { pub i: I, pub arr: [2]i32, pub skip: i32 };
            pub const main = fn(): i32 {
                let o = O{ .i = I{ .a = 1, .b = 2 }, .arr = [2]i32{ 3, 4 }, .skip = 0 };
                let .{ .i = .{ .b = |b| }, .arr = .{ _, |last| } } = o;
                return b * 10 + last;
            };
        )") == 24);
    }

    SECTION("the value runs once") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32, pub y: i32 };
            const make = fn(n: &mut i32): P {
                n = n + 1;
                return P{ .x = n, .y = n };
            };
            pub const main = fn(): i32 {
                let mut calls: i32 = 0;
                let .{ .x = |x|, .y = |y| } = make(&mut calls);
                return calls * 100 + x * 10 + y;
            };
        )") == 111);
    }

    SECTION("let mut makes mutable copies") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32 };
            pub const main = fn(): i32 {
                let p = P{ .x = 3 };
                let mut .{ .x = |x| } = p;
                x = x + 1;
                return p.x * 10 + x;
            };
        )") == 34);
    }

    SECTION("a type annotation guides the value") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32, pub y: i32 };
            pub const main = fn(): i32 {
                let .{ .y = |y| }: P = .{ .x = 1, .y = 2 };
                return y;
            };
        )") == 2);
    }

    SECTION("qualified patterns") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32 };
            pub const main = fn(): i32 {
                let P{ .x = |x| } = P{ .x = 5 };
                return x;
            };
        )") == 5);
    }

    SECTION("a one-variant union can't fail") {
        CHECK(helpers::compile_and_run(R"(
            const U = union(enum) { only: i32 };
            pub const main = fn(): i32 {
                let .only(|v|) = U{ .only = 6 };
                return v;
            };
        )") == 6);
    }
}

TEST_CASE("A destructured capture can alias its place") {
    SECTION("writing through a mutable reference") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32, pub y: i32 };
            pub const main = fn(): i32 {
                let mut p = P{ .x = 1, .y = 2 };
                let .{ .x = |&mut x|, .y = |y| } = p;
                x = 40;
                return p.x + y;
            };
        )") == 42);
    }

    SECTION("a pointer into an array") {
        CHECK(helpers::compile_and_run(R"(
            pub const main = fn(): i32 {
                let mut a = [2]i32{ 1, 2 };
                let .{ _, |^mut second| } = a;
                *second = 9;
                return a[1];
            };
        )") == 9);
    }

    SECTION("a read-only view of a temporary stays valid") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32 };
            const make = fn(): P { return P{ .x = 6 }; };
            pub const main = fn(): i32 {
                let .{ .x = |&x| } = make();
                return x + 1;
            };
        )") == 7);
    }

    SECTION("through a reference value") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32 };
            const f = fn(p: &mut P): void {
                let .{ .x = |&mut x| } = p;
                x = 8;
            };
            pub const main = fn(): i32 {
                let mut p = P{ .x = 1 };
                f(&mut p);
                return p.x;
            };
        )") == 8);
    }
}

TEST_CASE("let-else takes a pattern that can fail") {
    SECTION("binding on success, leaving on failure") {
        CHECK(helpers::compile_and_run(R"(
            const O = union(enum) { some: i32, none: void };
            const f = fn(o: O): i32 {
                let .some(|v|) = o else { return -1; };
                return v * 2;
            };
            pub const main = fn(): i32 { return f(O{ .some = 3 }) + f(O{ .none = {} }) + 1; };
        )") == 6);
    }

    SECTION("a jump with no block") {
        CHECK(helpers::compile_and_run(R"(
            const O = union(enum) { some: i32, none: void };
            pub const main = fn(): i32 {
                let mut total: i32 = 0;
                for ([3]O{ O{ .some = 1 }, O{ .none = {} }, O{ .some = 2 } }) |item| {
                    let .some(|v|) = item else continue;
                    total = total + v;
                }
                return total;
            };
        )") == 3);
    }

    SECTION("a refutable value test") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub tag: i32, pub v: i32 };
            const f = fn(p: P): i32 {
                let .{ .tag = 1, .v = |v| } = p else return 0;
                return v;
            };
            pub const main = fn(): i32 {
                return f(P{ .tag = 1, .v = 5 }) * 10 + f(P{ .tag = 2, .v = 5 });
            };
        )") == 50);
    }
}

TEST_CASE("Compile-time declarations destructure a folded value") {
    SECTION("const") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32, pub y: i32 };
            pub const main = fn(): i32 {
                const .{ .x = |x|, .y = |y| } = P{ .x = 2, .y = 3 };
                const arr = [x + y]i32{ 1, 2, 3, 4, 5 };
                return arr[4];
            };
        )") == 5);
    }

    SECTION("a type field") {
        CHECK(helpers::compile_and_run(R"(
            const elem_size = fn(comptime T: type): i32 {
                const .{ .child = |C| } = @typeInfo(T).pointer;
                return @intCast(i32, @sizeOf(C));
            };
            pub const main = fn(): i32 { return elem_size(^u16); };
        )") == 2);
    }

    SECTION("in a function folded at compile time") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32, pub y: i32 };
            const f = fn(p: P): i32 {
                let mut .{ .x = |x|, .y = |y| } = p;
                x = x + y;
                return x;
            };
            const K: i32 = f(P{ .x = 2, .y = 3 });
            pub const main = fn(): i32 { return K; };
        )") == 5);
    }

    SECTION("comptime let mut") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32 };
            pub const main = fn(): i32 {
                comptime let mut .{ .x = |x| } = P{ .x = 1 };
                comptime { x = x + 1; }
                return x;
            };
        )") == 2);
    }
}

TEST_CASE("Loop and branch captures destructure their value") {
    SECTION("for") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32, pub y: i32 };
            pub const main = fn(): i32 {
                let points = [2]P{ P{ .x = 1, .y = 2 }, P{ .x = 3, .y = 4 } };
                let mut total: i32 = 0;
                for (points) |.{ .x = |x|, .y = |y| }| { total = total + x * y; }
                return total;
            };
        )") == 14);
    }

    SECTION("for with break, continue, and an index") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32 };
            pub const main = fn(): i32 {
                let points = [4]P{ P{ .x = 1 }, P{ .x = 2 }, P{ .x = 3 }, P{ .x = 4 } };
                let mut total: i32 = 0;
                for (points, 0..4) |.{ .x = |x| }, i| {
                    if (x == 2) { continue; }
                    if (i == 3) { break; }
                    total = total + x;
                }
                return total;
            };
        )") == 4);
    }

    SECTION("for writes through an element") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32 };
            pub const main = fn(): i32 {
                let points = [2]mut P{ P{ .x = 1 }, P{ .x = 2 } };
                for (points) |.{ .x = |&mut x| }| { x = x * 10; }
                return points[0].x + points[1].x;
            };
        )") == 30);
    }

    SECTION("unroll for") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32 };
            pub const main = fn(): i32 {
                const points = [2]P{ P{ .x = 4 }, P{ .x = 5 } };
                let mut total: i32 = 0;
                unroll for (points) |.{ .x = |x| }| { total = total + x; }
                return total;
            };
        )") == 9);
    }
}

TEST_CASE("Destructuring reaches through an unwrap") {
    constexpr std::string_view types{R"(
        const Result = fn(comptime T: type, comptime E: type): type {
            return union(enum) { ok: T, err: E };
        };
        impl(T: type, E: type) builtin.Unwrappable for Result(T, E) {
            const Output = T;
            const Residual = E;
            pub const branch = fn(&mut? self): builtin.Flow(&mut? T, E) {
                return match (self) {
                    .ok => |&mut? v| .{ .@"continue" = v },
                    .err => |e| .{ .@"break" = e },
                };
            };
        }
        const Option = fn(comptime T: type): type {
            return union(enum) { some: T, none: void };
        };
        impl(T: type) builtin.Unwrappable for Option(T) {
            const Output = T;
            const Residual = void;
            pub const branch = fn(&mut? self): builtin.Flow(&mut? T, void) {
                return match (self) {
                    .some => |&mut? v| .{ .@"continue" = v },
                    .none => .{ .@"break" = {} },
                };
            };
        }
        const P = struct { pub x: i32, pub y: i32 };
        const Opt = Option(P);
        const Res = Result(P, P);
    )"};

    SECTION("if") {
        CHECK(helpers::compile_and_run(std::string{types} + R"(
            pub const main = fn(): i32 {
                let o = Opt{ .some = P{ .x = 2, .y = 3 } };
                let a = if (o) |.{ .x = |x|, .y = |y| }| x * y else 0;
                let n = Opt{ .none = {} };
                let b = if (n) |.{ .x = |x| }| x else 100;
                return a + b;
            };
        )") == 106);
    }

    SECTION("if with an else capture") {
        CHECK(helpers::compile_and_run(std::string{types} + R"(
            pub const main = fn(): i32 {
                let r = Res{ .err = P{ .x = 7, .y = 8 } };
                if (r) |.{ .x = |_x| }| {
                    return 1;
                } else |.{ .y = |y| }| {
                    return y;
                }
            };
        )") == 8);
    }

    SECTION("a payload written in place") {
        CHECK(helpers::compile_and_run(std::string{types} + R"(
            pub const main = fn(): i32 {
                let mut o = Opt{ .some = P{ .x = 2, .y = 3 } };
                if (o) |.{ .x = |&mut x| }| { x = 20; }
                return match (o) { .some(.{ .x = |x| }) => x, .none => 0 };
            };
        )") == 20);
    }
}

TEST_CASE("Module-level declarations destructure a constant") {
    SECTION("const") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32, pub y: i32 };
            const .{ .x = |X|, .y = |Y| } = P{ .x = 2, .y = 5 };
            pub const main = fn(): i32 { return X * Y; };
        )") == 10);
    }

    SECTION("used before it's written") {
        CHECK(helpers::compile_and_run(R"(
            pub const main = fn(): i32 { return A; };
            const P = struct { pub a: i32 };
            const .{ .a = |A| } = P{ .a = 6 };
        )") == 6);
    }

    SECTION("let mut gets separate storage per name") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32, pub y: i32 };
            let mut .{ .x = |gx|, .y = |gy| } = P{ .x = 1, .y = 2 };
            pub const main = fn(): i32 {
                gx = 10;
                return gx + gy;
            };
        )") == 12);
    }

    SECTION("arrays and types") {
        CHECK(helpers::compile_and_run(R"(
            const .{ |A|, |B| } = [2]i32{ 3, 4 };
            const .{ .child = |C| } = @typeInfo(^u32).pointer;
            pub const main = fn(): i32 { return A + B + @intCast(i32, @sizeOf(C)); };
        )") == 11);
    }
}

TEST_CASE("Destructured names cross modules with pub") {
    const auto imports{helpers::make_vector<helpers::mock_file>(helpers::mock_file{"cfg.gh",
                                                                                   R"(
            const P = struct { pub x: i32, pub y: i32 };
            pub const .{ .x = |X| } = P{ .x = 7, .y = 8 };
            const .{ .y = |HIDDEN| } = P{ .x = 7, .y = 8 };
        )",
                                                                                   "cfg"})};
    CHECK_FALSE(helpers::raised("import cfg; pub const main = fn(): i32 { return cfg.X; };",
                                sema::error::ILLEGAL_PRIVATE_ACCESS,
                                imports));
    CHECK(helpers::raised("import cfg; pub const main = fn(): i32 { return cfg.HIDDEN; };",
                          sema::error::ILLEGAL_PRIVATE_ACCESS,
                          imports));
}

TEST_CASE("Destructuring rejects what can't work") {
    constexpr auto ILLEGAL{sema::error::ILLEGAL_MATCH_PATTERN};

    SECTION("a pattern that can fail needs an else") {
        CHECK(rejects("let .ok(|v|) = r; return 0;", ILLEGAL));
        CHECK(rejects("let .{ .x = 0, .y = |y| } = p; return y;", ILLEGAL));
        CHECK(rejects("for ([1]R{ r }) |.err(|e|)| { return e; } return 0;", ILLEGAL));
    }

    SECTION("an else has to leave") {
        CHECK(rejects("let .ok(|v|) = r else { let n: i32 = 0; _ = n; }; return 0;", ILLEGAL));
    }

    SECTION("a folded value that doesn't fit") {
        CHECK(rejects("const .ok(|v|) = R{ .err = 1 }; return 0;",
                      sema::error::COMPTIME_EVALUATION_FAILED));
    }

    SECTION("a mutable alias needs a mutable place") {
        DISCARD(helpers::expect_compile_error(fmt::format(
            "{}\npub const main = fn(): i32 {{ let .{{ .x = |&mut x| }} = p; return 0; }};",
            SHAPES)));
    }
}

TEST_CASE("Destructuring syntax errors") {
    const auto fails_to_parse{[](std::string_view body) {
        syntax::parser parser{body};
        parser.enable_default_wrappers();
        ghoti::arena arena;
        ast::AST     ast;
        return !parser.consume(ast, arena).empty();
    }};

    CHECK(fails_to_parse("pub const main = fn(): i32 { const .{ .x = |&x| } = p; return 0; };"));
    CHECK(fails_to_parse("const .{ .x = |^x| } = p;"));
    CHECK(fails_to_parse("let .{ .x = |x| };"));
    CHECK(fails_to_parse("extern(\"c\") let .{ .x = |x| } = p;"));
    CHECK(fails_to_parse("@[align(4)] let .{ .x = |x| } = p;"));
    CHECK(fails_to_parse(
        "pub const main = fn(): i32 { match (a) { .b => |.{ .c = |c| }| c }; return 0; };"));
    CHECK(
        fails_to_parse("pub const main = fn(): i32 { match (a) { .b(||.c||) => 1 }; return 0; };"));
    CHECK(fails_to_parse("const S = struct { const .{ .x = |x| } = p; };"));
    CHECK(fails_to_parse("const .ok(|v|) = r else unreachable;"));
    // Inside a function an `else` has somewhere to go
    CHECK_FALSE(fails_to_parse(
        "pub const main = fn(): i32 { const .ok(|v|) = r else return 1; return v; };"));
}

TEST_CASE("A rich match folds to what it runs") {
    const auto input{GENERATE(as<std::string_view>{},
                              "S{ .tag = 0, .o = O{ .none = {} }, .arr = [2]i32{ 0, 0 } }",
                              "S{ .tag = 1, .o = O{ .none = {} }, .arr = [2]i32{ 0, 0 } }",
                              "S{ .tag = 1, .o = O{ .some = 5 }, .arr = [2]i32{ 0, 0 } }",
                              "S{ .tag = 1, .o = O{ .some = -5 }, .arr = [2]i32{ 0, 0 } }",
                              "S{ .tag = 2, .o = O{ .some = 5 }, .arr = [2]i32{ 3, 0 } }",
                              "S{ .tag = 2, .o = O{ .some = 5 }, .arr = [2]i32{ 3, 4 } }",
                              "S{ .tag = 3, .o = O{ .none = {} }, .arr = [2]i32{ 9, 9 } }",
                              "S{ .tag = 9, .o = O{ .some = 1 }, .arr = [2]i32{ 9, 8 } }",
                              "S{ .tag = -1, .o = O{ .some = 1 }, .arr = [2]i32{ 1, 1 } }")};
    CAPTURE(input);
    CHECK(helpers::compile_and_run(fmt::format(R"(
        const O = union(enum) {{ some: i32, none: void }};
        const S = struct {{ pub tag: i32, pub o: O, pub arr: [2]i32 }};
        const f = fn(s: S): i32 {{
            return match (s) {{
                .{{ .tag = 0 }} => 1,
                .{{ .tag = 1, .o = .none }} => 2,
                .{{ .tag = 1, .o = .some(|v|) }} if v > 0 => v,
                .{{ .tag = 1 }} => 3,
                .{{ .tag = 2, .arr = .{{ 3, 0 }} }} => 4,
                .{{ .tag = 2, .arr = .{{ |a|, |b| }} }} => a + b,
                .{{ .tag = 3.., .o = .some(|v|) }} => v + 10,
                .{{ .arr = .{{ |a|, _ }} }} |whole| if whole.tag > 3 => a,
                _ => 0,
            }};
        }};
        const K: i32 = f({0});
        pub const main = fn(): i32 {{
            let mut s = {0};
            s.tag = s.tag + 0;
            return if (f(s) == K) 1 else 0;
        }};
    )",
                                               input)) == 1);
}

TEST_CASE("Guards decide between arms at runtime") {
    SECTION("a failed guard moves on to the next arm") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32, pub y: i32 };
            const f = fn(p: P): i32 {
                return match (p) {
                    .{ .x = |x|, .y = |y| } if x == y => 1,
                    .{ .x = 0 } => 2,
                    .{ .x = |x| } if x > 5 => 3,
                    _ => 4,
                };
            };
            pub const main = fn(): i32 {
                let ok = f(P{ .x = 2, .y = 2 }) == 1 and f(P{ .x = 0, .y = 1 }) == 2 and
                         f(P{ .x = 9, .y = 1 }) == 3 and f(P{ .x = 1, .y = 2 }) == 4;
                return if (ok) 1 else 0;
            };
        )") == 1);
    }

    SECTION("a guard covers every alternative of its arm") {
        CHECK(helpers::compile_and_run(R"(
            const U = union(enum) { a: i32, b: i32 };
            const f = fn(u: U): i32 {
                return match (u) {
                    .a(|v|), .b(|v|) if v > 0 => v,
                    _ => -1,
                };
            };
            pub const main = fn(): i32 {
                return f(U{ .a = 3 }) + f(U{ .b = 4 }) * 10 + f(U{ .a = -5 }) + 1;
            };
        )") == 43);
    }

    SECTION("guards on a value and a guarded catch-all") {
        CHECK(helpers::compile_and_run(R"(
            const f = fn(n: i32, on: bool): i32 {
                return match (n) {
                    1.. if on => 1,
                    _ if n == 0 => 2,
                    _ => 3,
                };
            };
            pub const main = fn(): i32 {
                return f(5, true) * 100 + f(0, false) * 10 + f(5, false);
            };
        )") == 123);
    }

    SECTION("a pointer guard is truthy") {
        CHECK(helpers::compile_and_run(R"(
            const f = fn(n: i32, p: ^i32): i32 {
                return match (n) { 1 if p => 1, _ => 2 };
            };
            pub const main = fn(): i32 {
                let v: i32 = 0;
                return f(1, ^v) * 10 + f(1, nullptr);
            };
        )") == 12);
    }

    SECTION("a guard runs once per candidate arm, in order") {
        CHECK(helpers::compile_and_run(R"(
            const bump = fn(n: &mut i32, result: bool): bool {
                n = n + 1;
                return result;
            };
            pub const main = fn(): i32 {
                let mut hits: i32 = 0;
                let mut x: i32 = 0;
                x = 2;
                let r = match (x) {
                    0 if bump(&mut hits, true) => 10,
                    2 if bump(&mut hits, false) => 20,
                    2 if bump(&mut hits, true) => 30,
                    _ => 40,
                };
                return r + hits;
            };
        )") == 32);
    }

    SECTION("a guard can read through an alias capture") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32 };
            pub const main = fn(): i32 {
                let mut p = P{ .x = 1 };
                match (p) {
                    .{ .x = |&mut x| } if x == 1 => { x = 7; },
                    _ => {},
                };
                return p.x;
            };
        )") == 7);
    }
}

TEST_CASE("Guards decide between arms at compile time") {
    SECTION("folding the same match as a runtime one") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32, pub y: i32 };
            const f = fn(p: P): i32 {
                return match (p) {
                    .{ .x = |x|, .y = |y| } if x == y => 1,
                    .{ .x = |x| } if x > 5 => 2,
                    _ => 3,
                };
            };
            const A: i32 = f(P{ .x = 2, .y = 2 });
            const B: i32 = f(P{ .x = 9, .y = 2 });
            const C: i32 = f(P{ .x = 1, .y = 2 });
            pub const main = fn(): i32 { return A * 100 + B * 10 + C; };
        )") == 123);
    }

    SECTION("match comptime selects past a false guard") {
        CHECK(helpers::compile_and_run(R"(
            const size = fn(comptime T: type): i32 {
                return match comptime (@typeInfo(T)) {
                    .int(.{ .bits = |b| }) if b > 16 => 3,
                    .int(.{ .bits = |b| }) if b > 8 => 2,
                    .int => 1,
                    _ => 0,
                };
            };
            pub const main = fn(): i32 {
                return size(u32) * 50 + size(u16) * 10 + size(u8);
            };
        )") == 171);
    }
}

TEST_CASE("Nested variant patterns reach through payloads") {
    CHECK(helpers::compile_and_run(R"(
        const O = union(enum) { some: i32, none: void };
        const R = union(enum) { ok: O, err: i32 };
        const f = fn(r: R): i32 {
            return match (r) {
                .ok(.some(|raw|)) => raw,
                .ok(.none), .err => 100,
            };
        };
        pub const main = fn(): i32 {
            return f(R{ .ok = O{ .some = 7 } }) + f(R{ .ok = O{ .none = {} } }) + f(R{ .err = 1 });
        };
    )") == 207);
}

TEST_CASE("Nested variants test each constructor on its own path") {
    SECTION("rows sharing a prefix") {
        CHECK(helpers::compile_and_run(R"(
            const B = union(enum) { a: i32, b: i32 };
            const O = union(enum) { x: B, y: B };
            const f = fn(o: O): i32 {
                return match (o) {
                    .x(.a(|v|)) => v,
                    .x(.b(|v|)) => v * 2,
                    .y(.a(|v|)) => v * 3,
                    .y(.b(|v|)) => v * 4,
                };
            };
            pub const main = fn(): i32 {
                return f(O{ .x = B{ .a = 1 } }) + f(O{ .x = B{ .b = 1 } }) +
                       f(O{ .y = B{ .a = 1 } }) + f(O{ .y = B{ .b = 1 } });
            };
        )") == 10);
    }

    SECTION("an earlier wildcard takes every variant it can") {
        CHECK(helpers::compile_and_run(R"(
            const B = union(enum) { a: i32, b: i32 };
            const O = union(enum) { x: B, y: B };
            const f = fn(o: O): i32 {
                return match (o) {
                    .x(_) => 1,
                    .y(.a(_)) => 2,
                    _ => 3,
                };
            };
            pub const main = fn(): i32 {
                return f(O{ .x = B{ .b = 0 } }) * 100 + f(O{ .y = B{ .a = 0 } }) * 10 +
                       f(O{ .y = B{ .b = 0 } });
            };
        )") == 123);
    }
}

TEST_CASE("Struct patterns test and bind named fields") {
    const auto* const point{R"(
        const P = struct { pub x: i32, pub y: i32 };
        const f = fn(p: P): i32 {
            return match (p) {
                .{ .x = 0, .y = 0 } => 1,
                .{ .x = 0, .y = |y| } => y,
                .{ .x = |x|, .y = |y| } => x * y,
            };
        };
    )"};

    SECTION("first match wins") {
        CHECK(helpers::compile_and_run(std::string{point} + R"(
            pub const main = fn(): i32 {
                return f(P{ .x = 0, .y = 0 }) + f(P{ .x = 0, .y = 5 }) * 10;
            };
        )") == 51);
    }

    SECTION("the general row binds both fields") {
        CHECK(helpers::compile_and_run(std::string{point} + R"(
            pub const main = fn(): i32 { return f(P{ .x = 3, .y = 4 }); };
        )") == 12);
    }

    SECTION("omitted fields are ignored") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32, pub y: i32 };
            pub const main = fn(): i32 {
                let p = P{ .x = 2, .y = 99 };
                return match (p) { .{ .x = 2 } => 5, _ => 6 };
            };
        )") == 5);
    }
}

TEST_CASE("Fixed arrays match by position") {
    CHECK(helpers::compile_and_run(R"(
        const f = fn(a: [3]i32): i32 {
            return match (a) {
                .{ 0, |b|, _ } => b,
                .{ |x|, _, |z| } => x + z,
            };
        };
        pub const main = fn(): i32 {
            return f([3]i32{ 0, 9, 1 }) + f([3]i32{ 2, 9, 5 }) * 10;
        };
    )") == 79);
}

TEST_CASE("A pattern refines a value and binds the whole of it") {
    CHECK(helpers::compile_and_run(R"(
        const P = struct { pub x: i32, pub y: i32 };
        const f = fn(p: P): i32 {
            return match (p) { .{ .x = 1.. } |w| => w.y, _ => 0 };
        };
        pub const main = fn(): i32 {
            return f(P{ .x = 3, .y = 8 }) + f(P{ .x = 0, .y = 8 });
        };
    )") == 8);
}

TEST_CASE("Alternatives bind the same names") {
    CHECK(helpers::compile_and_run(R"(
        const U = union(enum) { a: i32, b: i32, c: void };
        const f = fn(u: U): i32 {
            return match (u) { .a(|v|), .b(|v|) => v, .c => 0 };
        };
        pub const main = fn(): i32 { return f(U{ .b = 6 }) + f(U{ .c = {} }); };
    )") == 6);
}

TEST_CASE("Alias captures write through nested places") {
    SECTION("a struct field inside a variant") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32, pub y: i32 };
            const U = union(enum) { a: P, b: i32 };
            pub const main = fn(): i32 {
                let mut u = U{ .a = P{ .x = 1, .y = 2 } };
                match (u) {
                    .a(.{ .y = |&mut y| }) => { y = 40; },
                    .b => {},
                };
                return match (u) { .a(.{ .x = |x|, .y = |y| }) => x + y, .b(|v|) => v };
            };
        )") == 41);
    }

    SECTION("an array element through a pointer capture") {
        CHECK(helpers::compile_and_run(R"(
            pub const main = fn(): i32 {
                let mut a = [2]i32{ 1, 2 };
                match (a) {
                    .{ _, |^mut second| } => { *second = 9; },
                };
                return a[1];
            };
        )") == 9);
    }

    SECTION("a read-only view of a temporary") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32 };
            const make = fn(): P { return P{ .x = 6 }; };
            pub const main = fn(): i32 {
                return match (make()) { .{ .x = |&x| } => x + 1 };
            };
        )") == 7);
    }
}

TEST_CASE("A by-value capture is a copy of the value it matched") {
    CHECK(helpers::compile_and_run(R"(
        const U = union(enum) { a: i32 };
        pub const main = fn(): i32 {
            let mut u = U{ .a = 1 };
            let mut seen: i32 = 0;
            match (u) {
                .a(|v|) => {
                    u = U{ .a = 50 };
                    seen = v;
                },
            };
            return seen;
        };
    )") == 1);
}

TEST_CASE("Patterns look through a reference but not a pointer") {
    SECTION("a reference field") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub v: i32 };
            const W = struct { pub r: &P };
            const f = fn(w: W): i32 {
                return match (w) {
                    .{ .r = .{ .v = 0 } } => 1,
                    .{ .r = .{ .v = |v| } } => v,
                };
            };
            pub const main = fn(): i32 {
                let p = P{ .v = 9 };
                return f(W{ .r = &p });
            };
        )") == 9);
    }

    SECTION("a reference scrutinee") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub v: i32 };
            const f = fn(p: &mut P): i32 {
                match (p) { .{ .v = |&mut v| } => { v = v + 1; } };
                return p.v;
            };
            pub const main = fn(): i32 {
                let mut p = P{ .v = 4 };
                return f(&mut p);
            };
        )") == 5);
    }

    SECTION("a pointer field stays a pointer") {
        CHECK(helpers::compile_and_run(R"(
            const W = struct { pub p: ^i32 };
            const f = fn(w: W): i32 {
                return match (w) {
                    .{ .p = nullptr } => 0,
                    .{ .p = |p| } => *p,
                };
            };
            pub const main = fn(): i32 {
                let v: i32 = 8;
                return f(W{ .p = ^v }) + f(W{ .p = nullptr });
            };
        )") == 8);
    }
}

TEST_CASE("Qualified patterns name the type they match") {
    CHECK(helpers::compile_and_run(R"(
        const S = union(enum) { circle: i32, square: i32 };
        const P = struct { pub a: i32 };
        const g = fn(p: P): i32 { return match (p) { P{ .a = |a| } => a }; };
        const f = fn(s: S): i32 {
            return match (s) { S.circle(|r|) => r, S.square(|w|) => w * 2 };
        };
        pub const main = fn(): i32 { return f(S{ .square = 4 }) + g(P{ .a = 1 }); };
    )") == 9);
}

TEST_CASE("Ranges decide exhaustiveness") {
    SECTION("covering a byte needs no catch-all") {
        CHECK(helpers::compile_and_run(R"(
            const f = fn(x: u8): i32 { return match (x) { 0..=127 => 1, 128..=255 => 2 }; };
            pub const main = fn(): i32 {
                let mut v: u8 = 0;
                v = 200;
                return f(v) * 10 + f(3u8);
            };
        )") == 21);
    }

    SECTION("open ends reach the type's limits") {
        CHECK(helpers::compile_and_run(R"(
            const f = fn(x: i8): i32 { return match (x) { ..0 => 1, 0 => 2, 1.. => 3 }; };
            pub const main = fn(): i32 {
                let mut lo: i8 = 0;
                lo = -128;
                let mut hi: i8 = 0;
                hi = 127;
                return f(lo) * 100 + f(0i8) * 10 + f(hi);
            };
        )") == 123);
    }

    SECTION("partial overlap takes the first arm") {
        CHECK(helpers::compile_and_run(R"(
            const f = fn(x: i32): i32 { return match (x) { 1..5 => 1, 3..8 => 2, _ => 3 }; };
            pub const main = fn(): i32 {
                let mut v: i32 = 0;
                v = 4;
                return f(v) * 100 + f(6) * 10 + f(9);
            };
        )") == 123);
    }

    SECTION("wide integers") {
        CHECK(helpers::compile_and_run(R"(
            const f = fn(x: u256): i32 {
                return match (x) {
                    0x10000000000000000000000000000000000 => 1,
                    0..0x10000000000000000000000000000000000 => 2,
                    _ => 3,
                };
            };
            pub const main = fn(): i32 {
                let big: u256 = 0x10000000000000000000000000000000000;
                return f(big) * 10 + f(5);
            };
        )") == 12);
    }
}

TEST_CASE("Top-level runtime patterns stay lazy and in order") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut hits: i32 = 0;
            const probe = fn(h: &mut i32, v: i32): i32 {
                h = h + 1;
                return v;
            };
            let mut n: i32 = 0;
            n = 2;
            let r = match (n) {
                probe(&mut hits, 1) => 10,
                probe(&mut hits, 2) => 20,
                probe(&mut hits, 3) => 30,
                _ => 40,
            };
            return r + hits;
        };
    )") == 22);
}

TEST_CASE("A generic pattern gets its types per instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const Box = fn(comptime T: type): type {
            return struct { pub v: T, pub ok: bool };
        };
        const get = fn(comptime T: type, b: Box(T), other: T): T {
            return match (b) {
                .{ .ok = true, .v = |v| } => v,
                .{ .ok = false } => other,
            };
        };
        pub const main = fn(): i32 {
            const small = get(i8, Box(i8){ .v = 1i8, .ok = true }, 2i8);
            const wide = get(i64, Box(i64){ .v = 3i64, .ok = false }, 4i64);
            return @as(i32, small) + @intCast(i32, wide);
        };
    )") == 5);
}

TEST_CASE("Bit-packed fields can be tested and copied") {
    CHECK(helpers::compile_and_run(R"(
        const F = packed struct { pub a: u3, pub b: u5 };
        const f = fn(p: F): i32 {
            return match (p) {
                .{ .a = 0 } => 1,
                .{ .a = |a|, .b = |b| } => @as(i32, a) * 10 + @as(i32, b),
            };
        };
        pub const main = fn(): i32 {
            return f(F{ .a = 2, .b = 7 });
        };
    )") == 27);
}

TEST_CASE("Nested patterns fold at compile time") {
    SECTION("the same match folded and at runtime") {
        CHECK(helpers::compile_and_run(R"(
            const O = union(enum) { some: i32, none: void };
            const P = struct { pub x: i32, pub o: O };
            const f = fn(p: P): i32 {
                return match (p) {
                    .{ .x = 0.., .o = .some(|v|) } => v,
                    _ => 2,
                };
            };
            const K: i32 = f(P{ .x = 1, .o = O{ .some = 5 } });
            pub const main = fn(): i32 {
                let mut q = P{ .x = 1, .o = O{ .some = 5 } };
                q.x = 1;
                return K * 10 + f(q);
            };
        )") == 55);
    }

    SECTION("an alias capture writes a compile-time place") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32 };
            const f = fn(): i32 {
                let mut p = P{ .x = 1 };
                match (p) { .{ .x = |&mut x| } => { x = 8; } };
                return p.x;
            };
            const K: i32 = f();
            pub const main = fn(): i32 { return K; };
        )") == 8);
    }

    SECTION("arrays") {
        CHECK(helpers::compile_and_run(R"(
            const f = fn(a: [2]i32): i32 {
                return match (a) { .{ 1, |b| } => b, _ => 0 };
            };
            const K: i32 = f([2]i32{ 1, 6 });
            pub const main = fn(): i32 { return K; };
        )") == 6);
    }
}

TEST_CASE("match comptime reads nested patterns") {
    SECTION("binding a value from type info") {
        CHECK(helpers::compile_and_run(R"(
            const bits = fn(comptime T: type): i32 {
                return match comptime (@typeInfo(T)) {
                    .int(.{ .bits = |b| }) => @as(i32, b),
                    _ => 0,
                };
            };
            pub const main = fn(): i32 { return bits(u16) + bits(bool); };
        )") == 16);
    }

    SECTION("testing a value from type info") {
        CHECK(helpers::compile_and_run(R"(
            const kind = fn(comptime T: type): i32 {
                return match comptime (@typeInfo(T)) {
                    .int(.{ .bits = 8 }) => 1,
                    .int => 2,
                    _ => 3,
                };
            };
            pub const main = fn(): i32 {
                return kind(u8) * 100 + kind(u16) * 10 + kind(bool);
            };
        )") == 123);
    }

    SECTION("capturing a type from type info") {
        CHECK(helpers::compile_and_run(R"(
            const size = fn(comptime T: type): i32 {
                return match comptime (@typeInfo(T)) {
                    .pointer(.{ .child = |C| }) => @intCast(i32, @sizeOf(C)),
                    _ => 0,
                };
            };
            pub const main = fn(): i32 { return size(^u16) + size(bool); };
        )") == 2);
    }

    SECTION("testing a type from type info") {
        CHECK(helpers::compile_and_run(R"(
            const check = fn(comptime T: type): i32 {
                return match comptime (@typeInfo(T)) {
                    .pointer(.{ .child = u8 }) => 1,
                    .pointer => 2,
                    _ => 3,
                };
            };
            pub const main = fn(): i32 {
                return check(^u8) * 100 + check(^u16) * 10 + check(bool);
            };
        )") == 123);
    }
}

} // namespace ghoti::tests

namespace ghoti::tests {

TEST_CASE("Nested matches report exactly what they miss") {
    constexpr auto ILLEGAL{sema::error::ILLEGAL_MATCH_PATTERN};
    constexpr auto MISSING{sema::error::MISSING_FIELD};

    CHECK(rejects("return match (r) { .ok(.some(|v|)) => v, .err => 0 };", MISSING));
    CHECK(rejects("return match (p) { .{ .x = 0 } => 1 };", MISSING));
    CHECK(
        rejects("return match (r) { .ok(.none) => 1, .err(0) => 2, .err(1..) => 3, .ok(_) => 4 };",
                MISSING));

    SECTION("an arm an earlier one covers") {
        CHECK(rejects("return match (r) { .ok(_) => 1, .ok(.none) => 2, .err => 3 };", ILLEGAL));
        CHECK(rejects("return match (p) { .{ .x = |x| } => x, .{ .x = 0 } => 2 };", ILLEGAL));
        CHECK(rejects("let n: i32 = 1; return match (n) { 0..5 => 1, 3 => 2, _ => 0 };", ILLEGAL));
    }
}

TEST_CASE("Malformed nested patterns are rejected with a reason") {
    constexpr auto ILLEGAL{sema::error::ILLEGAL_MATCH_PATTERN};

    SECTION("variants") {
        CHECK(rejects("return match (r) { .ok |x| => 1, .err => 2 };", ILLEGAL));
        CHECK(rejects("return match (r) { .ok(_, _) => 1, .err => 2 };", ILLEGAL));
        CHECK(rejects("return match (r) { .ok() => 1, .err => 2 };", ILLEGAL));
        CHECK(rejects("return match (r) { .ok(.none(_)) => 1, _ => 2 };", ILLEGAL));
        CHECK(rejects("return match (r) { .ok(.nope) => 1, _ => 2 };", sema::error::UNKNOWN_FIELD));
    }

    SECTION("captures") {
        CHECK(rejects("return match (r) { .err(|v|) => |w| v, _ => 2 };", ILLEGAL));
        CHECK(rejects("return match (r) { .err(|v|), .ok(.some(|w|)) => 1, _ => 2 };", ILLEGAL));
        CHECK(rejects("return match (p) { .{ .x = |v|, .y = |v| } => v };", ILLEGAL));
        CHECK(rejects("let f = F{ .a = 0, .b = 0 }; return match (f) { .{ .a = |&a| } => 1 };",
                      ILLEGAL));
        CHECK(rejects("return match (p) { .{ .x = |&mut x| } => x };",
                      sema::error::ASSIGNMENT_TO_CONST));
    }

    SECTION("aggregates") {
        CHECK(rejects("return match (p) { .{ |a|, |b| } => a };", ILLEGAL));
        CHECK(rejects("return match (p) { .{ .x = 0, .x = 1 } => 1, _ => 2 };",
                      sema::error::DUPLICATE_FIELD));
        CHECK(
            rejects("return match (p) { .{ .z = 0 } => 1, _ => 2 };", sema::error::UNKNOWN_FIELD));
        CHECK(rejects("let a = [2]i32{ 1, 2 }; return match (a) { .{ |x| } => x };", ILLEGAL));
        CHECK(rejects("let a = [2]i32{ 1, 2 }; return match (a) { .{ .x = 1 } => 1, _ => 2 };",
                      ILLEGAL));
        CHECK(rejects("let n: i32 = 1; return match (n) { .{ .x = 1 } => 1, _ => 2 };", ILLEGAL));
    }

    SECTION("qualifiers name the matched type") {
        CHECK(rejects("return match (p) { O{ .x = 0 } => 1, _ => 2 };", ILLEGAL));
    }

    SECTION("nested leaves must be compile-time known") {
        CHECK(rejects("let mut n: i32 = 1; n = 2; return match (p) { .{ .x = n } => 1, _ => 2 };",
                      ILLEGAL));
        CHECK(
            rejects("let mut n: i32 = 1; n = 2; return match (p) { .{ .x = 0..n } => 1, _ => 2 };",
                    ILLEGAL));
    }

    SECTION("floats stay unmatchable at every depth") {
        CHECK(rejects("const Q = struct { pub v: f32 }; let q = Q{ .v = 1.0 };"
                      " return match (q) { .{ .v = 1.0 } => 1, _ => 2 };",
                      sema::error::TYPE_MISMATCH));
    }

    SECTION("a struct value has no equality to test") {
        CHECK(rejects("const W = struct { pub p: P }; let w = W{ .p = p };"
                      " return match (w) { .{ .p = p } => 1, _ => 2 };",
                      ILLEGAL));
    }

    SECTION("strings") {
        CHECK(rejects("let s: []u8 = \"a\"; return match (s) { \"a\" => 1 };",
                      sema::error::MISSING_FIELD));
        CHECK(
            rejects("let a = [2]u8{ 1, 2 }; return match (a) { \"abc\" => 1, _ => 2 };", ILLEGAL));
        CHECK(rejects("let a = [1]i32{ 1 }; return match (a) { \"a\" => 1, _ => 2 };", ILLEGAL));
        CHECK(rejects("let n: i32 = 1; return match (n) { \"a\" => 1, _ => 2 };", ILLEGAL));
    }

    SECTION("guards") {
        CHECK(rejects("return match (r) { .ok(_) if true => 1, .err => 2 };",
                      sema::error::MISSING_FIELD));
        CHECK(rejects("const T = i32; return match comptime (T) { i32 if true => 1, _ => 2 };",
                      sema::error::ILLEGAL_MATCH_PATTERN));
        CHECK(rejects(
            "let mut n: i32 = 1; n = 2; return match comptime (1) { 1 if n > 0 => 1, _ => 2 };",
            sema::error::COMPTIME_EVALUATION_FAILED));
    }

    SECTION("open ranges need a sized type") {
        CHECK(rejects("return match (1) { 0.. => 1, _ => 2 };", ILLEGAL));
    }
}

TEST_CASE("A guard can't write through an alias capture") {
    const auto guarded{[](std::string_view guard) {
        return fmt::format(R"(
            const P = struct {{ pub x: i32 }};
            const assign = fn(n: &mut i32): bool {{ n = 1; return true; }};
            pub const main = fn(): i32 {{
                let mut q = P{{ .x = 0 }};
                return match (q) {{ .{{ .x = |&mut x| }} if {} => 1, _ => 2 }};
            }};
        )",
                           guard);
    }};
    DISCARD(helpers::expect_compile_error(guarded("assign(x)")));
    DISCARD(helpers::expect_compile_error(guarded("(x = 5) == 5")));
}

TEST_CASE("A matched place is evaluated once however its arms capture") {
    const std::string_view arm{GENERATE("|v| => v", "|&v| => v", "1.. |v| => v, _ => 0")};
    CAPTURE(arm);
    CHECK(helpers::compile_and_run(fmt::format(R"(
        pub const main = fn(): i32 {{
            let a = [3]i32{{ 10, 20, 30 }};
            let mut calls: i32 = 0;
            const next = fn(c: &mut i32): usize {{ c = c + 1; return 1; }};
            let got = match (a[next(&mut calls)]) {{ {} }};
            return got + calls;
        }};
    )",
                                               arm)) == 21);
}

TEST_CASE("Private fields stay private to patterns") {
    CHECK_FALSE(helpers::raised(R"(
        const S = struct { hidden: i32 };
        pub const main = fn(): i32 {
            let s = S{ .hidden = 1 };
            return match (s) { .{ .hidden = |h| } => h };
        };
    )",
                                sema::error::ILLEGAL_PRIVATE_ACCESS));
}

} // namespace ghoti::tests
