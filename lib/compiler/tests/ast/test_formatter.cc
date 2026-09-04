#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "helpers/formatter.hh"

namespace ghoti::tests {

using helpers::format_source;
using helpers::round_trips;

TEST_CASE("formatter round-trips simple declarations") {
    CHECK(format_source(R"(const version := "0.0.1";)") == "const version := \"0.0.1\";\n");
    CHECK(format_source("pub  const   x:i32=42;") == "pub const x: i32 = 42;\n");
    CHECK(format_source("constexpr SIZE:=2uz;") == "constexpr SIZE := 2uz;\n");
    CHECK(format_source("var a := 3u32;") == "var a := 3u32;\n");
    CHECK(format_source("var a := 1i64;") == "var a := 1i64;\n");
    CHECK(format_source("var a := 2.3;") == "var a := 2.3;\n");
    CHECK(format_source("var a := 'a';") == "var a := 'a';\n");
}

TEST_CASE("formatter preserves numeric literal base, separators, and suffix verbatim") {
    CHECK(format_source("var a := 0x2Fuz;") == "var a := 0x2Fuz;\n");
    CHECK(format_source("var a := 0b00_11_00_11;") == "var a := 0b00_11_00_11;\n");
    CHECK(format_source("var a := 1_000_000;") == "var a := 1_000_000;\n");
    CHECK(format_source("var a := 0o17u64;") == "var a := 0o17u64;\n");
    CHECK(format_source("var a := 2.5e10;") == "var a := 2.5e10;\n");
    CHECK(format_source(R"(var a := '\n';)") == "var a := '\\n';\n");
}

TEST_CASE("formatter preserves grouping parens the author wrote") {
    CHECK(format_source("_ = (a + b) * c;") == "_ = (a + b) * c;\n");
    CHECK(format_source("_ = a + b * c;") == "_ = a + b * c;\n");
    CHECK(format_source("_ = ((a));") == "_ = ((a));\n");
    CHECK(format_source("_ = (*arr[i][j]);") == "_ = (*arr[i][j]);\n");
    CHECK(format_source("const x := (a);") == "const x := (a);\n");
}

TEST_CASE("formatter round-trips operator expressions") {
    CHECK(format_source("a <= b or c == d and e;") == "a <= b or c == d and e;\n");
    CHECK(format_source("a or b[3uz] == !c;") == "a or b[3uz] == !c;\n");
    CHECK(format_source("A::B::C;") == "A::B::C;\n");
    CHECK(format_source("a.b;") == "a.b;\n");
    CHECK(format_source("a..b;") == "a..b;\n");
    CHECK(format_source("a..=b;") == "a..=b;\n");
    CHECK(format_source("&a; &mut b; *a; ^mut c;") == R"(&a;
&mut b;
*a;
^mut c;
)");
    CHECK(format_source("i += 1;") == "i += 1;\n");
}

TEST_CASE("formatter round-trips postfix unwrap operators") {
    CHECK(format_source("a?;") == "a?;\n");
    CHECK(format_source("a!;") == "a!;\n");
    CHECK(format_source("foo(x)?;") == "foo(x)?;\n");

    // `?` and `!` bind tighter than `.`, so no parens are re-inserted
    CHECK(format_source("a.b?;") == "a.b?;\n");
    CHECK(format_source("a?.b;") == "a?.b;\n");
    CHECK(format_source("foo()?.bar!;") == "foo()?.bar!;\n");
    CHECK(format_source("_ = a? + b;") == "_ = a? + b;\n");

    // author-written grouping parens survive
    CHECK(format_source("(a + b)?;") == "(a + b)?;\n");
}

TEST_CASE("formatter round-trips leaf statements") {
    CHECK(format_source("import std;") == "import std;\n");
    CHECK(format_source(R"(pub import "ast/node.p" as node;)") ==
          "pub import \"ast/node.p\" as node;\n");
    CHECK(format_source("using T = i32;") == "using T = i32;\n");
    CHECK(format_source("pub using a = ^^i32;") == "pub using a = ^^i32;\n");
    CHECK(format_source("break :blk a;") == "break :blk a;\n");
    CHECK(format_source("continue;") == "continue;\n");
    CHECK(format_source("return enum { RED };") == "return enum { RED };\n");
    CHECK(format_source("_ = enum { RED, _ };") == "_ = enum { RED, _ };\n");
    CHECK(format_source("defer 3;") == "defer 3;\n");
}

TEST_CASE("formatter round-trips types") {
    CHECK(format_source("var a: []i32 = undefined;") == "var a: []i32 = undefined;\n");
    CHECK(format_source("var a: std::ArrayList(u8) = undefined;") ==
          "var a: std::ArrayList(u8) = undefined;\n");
    CHECK(format_source("var a: List(i32) = undefined;") == "var a: List(i32) = undefined;\n");
    CHECK(format_source("var v: mut volatile i32 = 42;") == "var v: mut volatile i32 = 42;\n");
    CHECK(format_source("var f: ^fn(x: &a, y: ^mut B, ...): ^E = undefined;") ==
          "var f: ^fn(x: &a, y: ^mut B, ...): ^E = undefined;\n");
    CHECK(format_source("var a: [N:0]u8 = undefined;") == "var a: [N:0]u8 = undefined;\n");
    CHECK(format_source("var w: &dyn Writer = undefined;") == "var w: &dyn Writer = undefined;\n");
    CHECK(format_source("var w: ^mut dyn io::Writer = undefined;") ==
          "var w: ^mut dyn io::Writer = undefined;\n");
    CHECK(format_source("var it: &dyn Iterator(Item = u8) = undefined;") ==
          "var it: &dyn Iterator(Item = u8) = undefined;\n");
    CHECK(format_source("var m: &dyn Map(Key = []u8, Value = i32) = undefined;") ==
          "var m: &dyn Map(Key = []u8, Value = i32) = undefined;\n");
}

TEST_CASE("formatter lays out a function body with K&R braces") {
    CHECK(format_source("pub const main := fn(): i32 { return 0; };") ==
          R"(pub const main := fn(): i32 {
    return 0;
};
)");
    CHECK(format_source("const f := fn(): void {};") == "const f := fn(): void {};\n");
}

TEST_CASE("formatter round-trips a constexpr parameter modifier") {
    CHECK(format_source("const f := fn(constexpr n: i32, x: i32): i32 { return x; };") ==
          "const f := fn(constexpr n: i32, x: i32): i32 {\n    return x;\n};\n");
}

TEST_CASE("formatter keeps small aggregates inline and breaks ones with bodies") {
    CHECK(format_source("const P := struct { x: i32, y: i32 };") ==
          "const P := struct { x: i32, y: i32 };\n");
    CHECK(format_source("const U := union { a: i32, b: i32 };") ==
          "const U := union { a: i32, b: i32 };\n");
    CHECK(format_source("const E := enum : u64 { A = 1u64, B, C };") ==
          "const E := enum : u64 { A = 1u64, B, C };\n");

    CHECK(format_source("const S := struct { x: i32, const m := fn(): i32 { return x; }; };") ==
          R"(const S := struct {
    x: i32,

    const m := fn(): i32 {
        return x;
    };
};
)");
}

TEST_CASE("formatter puts a hard line after top level functions and structs") {
    CHECK(format_source("const f := fn(): void {};\nconst g := fn(): void {};") ==
          R"(const f := fn(): void {};

const g := fn(): void {};
)");

    CHECK(format_source(
              "test \"yeah this is epic\"{ @expect(a == b); }\nconst g := fn(): void {};") ==
          R"(test "yeah this is epic" {
    @expect(a == b);
}

const g := fn(): void {};
)");

    CHECK(format_source("pub const main := fn(): i32 { return 0; };\nconst x := 42;") ==
          R"(pub const main := fn(): i32 {
    return 0;
};

const x := 42;
)");

    CHECK(format_source("const S := struct { x: i32, };\nconst f := fn(): void {};") ==
          R"(const S := struct { x: i32 };

const f := fn(): void {};
)");

    CHECK(format_source("const a := 1;\nconst b := 2;") == "const a := 1;\nconst b := 2;\n");
}

TEST_CASE("formatter puts a hard line between fields and members in aggregates") {
    CHECK(format_source("const U := union { a: i32, const b := fn(&self, a: A): C { c; }; };") ==
          R"(const U := union {
    a: i32,

    const b := fn(&self, a: A): C {
        c;
    };
};
)");

    CHECK(format_source(
              "const E := enum : i64 { A = 2i64, const b := fn(&self, a: A): C { c; }; };") ==
          R"(const E := enum : i64 {
    A = 2i64,

    const b := fn(&self, a: A): C {
        c;
    };
};
)");
}

TEST_CASE("formatter lays out if / match") {
    CHECK(format_source("if (a) { b(); } else { c(); };") == R"(if (a) {
    b();
} else {
    c();
};
)");

    CHECK(format_source("const r := match (u) { .a => 1, .b => 2 };") ==
          "const r := match (u) { .a => 1, .b => 2 };\n");

    CHECK(format_source("const r := match (n) { 1..8 => 1, _ => 0 };") ==
          "const r := match (n) { 1..8 => 1, _ => 0 };\n");

    CHECK(format_source("const r := match (n) { 1, 2, 3 => 1, _ => 0 };") ==
          "const r := match (n) { 1, 2, 3 => 1, _ => 0 };\n");

    CHECK(format_source("const r := match constexpr (T) { i32 => 1, _ => 0 };") ==
          "const r := match constexpr (T) { i32 => 1, _ => 0 };\n");

    CHECK(format_source("match (u) { .a => |&mut v| { v = 1; }, _ => {}, };") == R"(match (u) {
    .a => |&mut v| {
        v = 1;
    },
    _ => {},
};
)");
}

TEST_CASE("formatter does not double the terminator on a value-if or loop tail") {
    CHECK(
        format_source("const min := fn(a: auto, b: auto): auto { return if (a < b) a else b; };") ==
        R"(const min := fn(a: auto, b: auto): auto {
    return if (a < b) a else b;
};
)");
    CHECK(format_source("while (c) { a; } else return b;") == R"(while (c) {
    a;
} else return b;
)");
    CHECK(format_source("if (c) return x; else return y;") == "if (c) return x; else return y;\n");
}

TEST_CASE("formatter writes an inferred array size as _") {
    CHECK(format_source("[_]i32{};") == "[_]i32{};\n");
    CHECK(format_source("[_:0]^N{ a, b };") == "[_:0]^N{ a, b };\n");
}

TEST_CASE("formatter breaks a wide argument list") {
    const auto out{format_source(
        "callee(aaaaaaaaaa, bbbbbbbbbb, cccccccccc, dddddddddd, eeeeeeeeee, ffffffffff);", 40)};
    CHECK(out == R"(callee(
    aaaaaaaaaa,
    bbbbbbbbbb,
    cccccccccc,
    dddddddddd,
    eeeeeeeeee,
    ffffffffff,
);
)");
}

TEST_CASE("formatter preserves a single blank line between items") {
    CHECK(format_source("const a := 1;\n\n\nconst b := 2;") == R"(const a := 1;

const b := 2;
)");
    CHECK(format_source("const a := 1;\nconst b := 2;") == "const a := 1;\nconst b := 2;\n");
    CHECK(format_source("const f := fn(): void { a();\n\n\n b(); };") == R"(const f := fn(): void {
    a();

    b();
};
)");
}

TEST_CASE("formatter round trip: declarations and literals") {
    round_trips(R"(pub const version := "0.0.1";)");
    round_trips("constexpr SIZE := 2uz;");
    round_trips("var a: i32 = undefined;");
    round_trips("var v: mut volatile i32 = 42;");
    round_trips("const v: volatile i32 = 42;");
    round_trips("var a := 0x2Fuz; var b := 0b00_11_00_11; var c := 1_000; var d := 2.3f32;");
    round_trips(R"('\n'; '\r'; '\t'; '\\'; '\''; '\0';)");
}

TEST_CASE("formatter round trip: operators and grouping") {
    round_trips("a <= b or c == d and e;");
    round_trips("a or b[3uz] == !c;");
    round_trips("(*arr[i][j]) = 2;");
    round_trips("_ = (a + b) * c;");
    round_trips("_ = a + b * c - d / e;");
    round_trips("_ = ((a));");
    round_trips("A::B::C; a.b; a..b; a..=b;");
    round_trips("&a; &mut b; *a; ^mut a; ^a;");
    round_trips("@as(i32, a); .a; .{ .a = 3 }; TT{ .adfasf = a }; .{};");
    round_trips("_ = a +% b - c *% d + e <<% f;");
    round_trips("_ = -%a;");
    round_trips("var x: u8 = 0; x +%= 1; x -%= 1; x *%= 2; x <<%= 1;");
}

TEST_CASE("formatter round trip: precedence and nesting are preserved") {
    round_trips("_ = a + b * c - d;");
    round_trips("_ = (a + b) * (c - d);");
    round_trips("_ = a and b or c and d;");
    round_trips("_ = a or (b or c) or d;");
    round_trips("_ = a - b - c;");
    round_trips("_ = a - (b - c);");
    round_trips("_ = !a and !(b or c);");
    round_trips("_ = *a.b[0].c;");
    round_trips("_ = &obj.field;");
    round_trips("_ = a.b.c().d[e].f;");
    round_trips("_ = (a + 1)..(b - 1);");
    round_trips("_ = x == y and (p or q);");
}

TEST_CASE("formatter round trip: functions and types") {
    round_trips("var f_ptr: ^fn(x: &a, y: ^mut B, ...): &[0x2uz][N]^E = undefined;");
    round_trips("fn(^mut this, a: A, b: ^B, ): i32 { c; };");
    round_trips("fn(self): i32 {};");
    round_trips("pub const min := fn(a: auto, b: auto): auto { return if (a < b) a else b; };");
    round_trips("using T = i32; pub using a = ^^i32;");
    round_trips("var a: std::ArrayList(u8) = undefined; var a: List(i32) = undefined; var a: []i32 "
                "= undefined;");
    round_trips("extern const foo: fn(): i32;");
    round_trips(R"(extern("kernel32") const bar: fn(): void;)");
    round_trips(R"(extern("c", "__errno_location") const errno_loc: fn(): ^mut i32;)");
    round_trips(R"(export("ghoti_add") const add := fn(a: i32, b: i32): i32 { return a + b; };)");
    round_trips("threadlocal var tls_counter: i32 = 0;");
    round_trips("pub threadlocal var tls_state: i64 = 0i64;");
    round_trips("weak extern const maybe: fn(): void;");
    round_trips("pub weak const overridable := fn(): i32 { return 1; };");
    round_trips("@discardable extern const puts: fn(s: ^u8): i32;");
    round_trips("pub @discardable const log := fn(msg: i32): i32 { return msg; };");
    round_trips("pub const stub := naked fn(): void {};");
    round_trips("pub const handler := fn() callconv(.win64): void {};");
    round_trips("const cb := fn(x: i32) callconv(.stdcall): i32 { return x; };");
}

TEST_CASE("formatter round trip: aggregates") {
    round_trips("struct { var a: Foo = bar; const b := fn(^mut this, a: A, b: ^B): C { c; }; };");
    round_trips("union { a: i32, b: &mut T, };");
    round_trips("enum : u64 { A = 1u64, B = T, C, };");
    round_trips("enum : i64 { A = 2i64, const b := fn(&self, a: A): C { c; }; };");
    round_trips("union { a: struct { b: Foo = bar, pub c: i32, var d: u32 = undefined; }, "
                "const b := fn(&self, a: A): C { c; }; };");
    round_trips(R"(const S := struct {
    x: i32,
    const make := fn(v: auto): i32 {
        const r: @this() = S{ .x = v };
        return r.x;
    };
};)");
}

TEST_CASE("formatter keeps small interfaces inline and breaks ones with bodies") {
    CHECK(format_source("const M := interface {};") == "const M := interface {};\n");
    CHECK(
        format_source("const W := interface { pub const write := fn(&mut self, b: []u8): R; };") ==
        "const W := interface { pub const write := fn(&mut self, b: []u8): R; };\n");

    CHECK(format_source(
              "const W := interface { Error: type; const cap: usize = 4096; "
              "pub const write := fn(&mut self, b: []u8): R; "
              "const dbg := fn(&self): []u8; "
              "pub const writeAll := fn(&mut self, b: []u8): R { return self.write(b); }; };") ==
          R"(const W := interface {
    Error: type;
    const cap: usize = 4096;
    pub const write := fn(&mut self, b: []u8): R;
    const dbg := fn(&self): []u8;
    pub const writeAll := fn(&mut self, b: []u8): R {
        return self.write(b);
    };
};
)");
}

TEST_CASE("formatter round trips impl blocks with no trailing semicolon") {
    CHECK(
        format_source(
            "impl File { pub const fromRaw := fn(fd: i32): @this() { return .{ .fd = fd }; }; }") ==
        R"(impl File {
    pub const fromRaw := fn(fd: i32): @this() {
        return .{ .fd = fd };
    };
}
)");

    CHECK(format_source("impl Writer for File { pub const write := fn(&mut self, b: []u8): R "
                        "{ return os::write(self.fd, b); }; }") ==
          R"(impl Writer for File {
    pub const write := fn(&mut self, b: []u8): R {
        return os::write(self.fd, b);
    };
}
)");

    CHECK(format_source("impl(H: type) Writer(H) { pub const fromRaw := fn(raw: H): @this() "
                        "{ return .{ .handle = raw }; }; }") ==
          R"(impl(H: type) Writer(H) {
    pub const fromRaw := fn(raw: H): @this() {
        return .{ .handle = raw };
    };
}
)");
}

TEST_CASE("formatter puts a blank line between an impl block and adjacent items") {
    CHECK(format_source("impl A for B { pub const f := fn(&self): void {}; }\nconst x := 1;") ==
          R"(impl A for B {
    pub const f := fn(&self): void {};
}

const x := 1;
)");
}

TEST_CASE("formatter round trip: interfaces and impls") {
    round_trips("const M := interface {};");
    round_trips("const W := interface { Item: type = u8; const n: usize; "
                "pub const next := fn(&mut self): Item; "
                "const seal := fn(&self): void; "
                "pub const drain := fn(&mut self): void { self.seal(); }; };");
    round_trips("using X = interface { pub const f := fn(^self): i32; };");
    round_trips("impl File { pub const make := fn(): @this() { return .{}; }; }");
    round_trips("impl Writer for File { pub const write := fn(&mut self, b: []u8): R { c; }; }");
    round_trips("impl(T: type) Debug for Box(T) { pub const fmt := fn(&self): void {}; }");
    round_trips("impl(H: type, constexpr n: usize) Buf(H) { const cap := n; }");
}

TEST_CASE("formatter round trip: @cfg groups inside aggregate bodies") {
    round_trips("const S := struct { dev: u64, @cfg(os == .linux) { uid: u32, gid: u32 } "
                "mode: u32, };");
    round_trips("const S := struct { a: i32, @cfg(ptr_bits >= 32) wide: i64, b: i32, };");
    round_trips("const S := struct { a: i32, @cfg(os == .linux) { l: u8 } "
                "else @cfg(os == .macos) { m: u8 } else { o: u8 } z: i32, };");
    round_trips("const U := union { @cfg(ptr_bits == 64) { a: u64 } else { a: u32 } };");
    round_trips("const E := enum { A, @cfg(os == .windows) { B, C } @cfg(os == .linux) D, F, };");
}

TEST_CASE("formatter round trip: @cfg groups gating aggregate members") {
    round_trips("const S := struct { a: i32, const k := 1; "
                "@cfg(os == .linux) { const l := fn(): i32 { return 1; }; } };");
    round_trips("const S := struct { a: i32, @cfg(ptr_bits == 64) { using W = u64; } "
                "else { using W = u32; } };");
    round_trips("const E := enum { A, @cfg(os == .linux) { const tag := 1; } "
                "else { const tag := 2; } };");
}

TEST_CASE("formatter round trip: control flow") {
    round_trips("if (a) { b; } else { c; };");
    round_trips("if constexpr (a) { b; };");
    round_trips("while (true) : (i += 1) { a; } else return b;");
    round_trips("do { a; } while (true);");
    round_trips("for (arr, l, p) |i, &mut j, _| { a; } else return b;");
    round_trips("loop { a; };");
    round_trips("match (a) { b => |c| d, e => |_| f, g => h, _ => d, };");
    round_trips("match (n) { 1..10 => |v| v, 10..=20 => 2, _ => 0 };");
    round_trips("match (n) { 1, 2, 5..9 => |v| v, _ => 0 };");
    round_trips("match constexpr (T) { i32 => 1, i64 => 2, _ => 0 };");
    round_trips("match constexpr (n) { 1, 2 => |v| v, _ => 0 };");
    round_trips("a: { continue :a; };");
    round_trips(R"(test "dump" { import other; @expect(a == true); })");
}

TEST_CASE("formatter round trip: nested module") {
    round_trips(R"(pub const main := fn(): i32 {
    var u := U{ .b = 7 };
    for (0..3) |v| { sum = sum + v; }
    return match (u) { .a => 1, .b => 2, };
};)");
}

TEST_CASE("formatter round trip: adjacent brace-tailed statements keep their terminators") {
    round_trips(".{ .a = 3 }; TT{ .adfasf = a }; .{};");
    round_trips("[1uz]A{a}; [_]i32{}; b.c;");
    round_trips("if (a) { b; }; .c;");
    round_trips("match (a) { _ => d, }; x[0];");
    round_trips("struct { a: i32 }; .field;");
}

TEST_CASE("formatter preserves leading, trailing, and in-block comments") {
    constexpr std::string_view source{R"(// File leading comment
const X := 1;
// leading comment
pub const main := fn(): i32 { // trailing on brace
    // comment before decl
    var sum: i32 = 0; // trailing
    // comment between statements
    if (sum == 0) { // comment in if header context
        sum = sum + 1;
        // comment before closing brace
    }
    for (0..3) |v| { // comment in for header
        sum = sum + v;
    }
    match (sum) {
        // comment inside match
        4 => {
            sum = sum + 100;
        },
        _ => {},
    }
    return sum;
    // trailing comment before closing brace
};
)"};

    CHECK(format_source(source) == source);
}

TEST_CASE("formatter preserves comments inside aggregates") {
    constexpr std::string_view source{R"(const S := struct {
    // comment before field
    x: i32, // field trailing

    // comment before member
    const m := fn(): i32 {
        return x;
    };
};
)"};

    CHECK(format_source(source) == source);
}

TEST_CASE("formatter preserves standalone comments") {
    CHECK(format_source("// single line comment\n") == "// single line comment\n");
    CHECK(format_source("// comment 1\n// comment 2\n") == "// comment 1\n// comment 2\n");
}

TEST_CASE("formatter does not insert extra newlines after trailing comments") {
    constexpr std::string_view source{R"(const a := 1; // comment a
const b := 2; // comment b
const c := 3; // comment c
)"};

    CHECK(format_source(source) == source);
}

TEST_CASE("formatter preserves trailing comments on statements without extra blank lines") {
    constexpr std::string_view source{R"(pub const foo := fn(): void {
    var x := 1; // comment on x
    var y := 2; // comment on y
    var z := 3;
    // comment on z
};
)"};

    CHECK(format_source(source) == source);
}

TEST_CASE("formatter keeps the blank line before a leading comment group") {
    constexpr std::string_view top_level{R"(const a: i32 = 2;

// some comment
const b: i32 = 3;
)"};
    CHECK(format_source(top_level) == top_level);

    constexpr std::string_view in_block{R"(const f := fn(): void {
    var a := 1;

    // some comment
    var b := 2;
};
)"};
    CHECK(format_source(in_block) == in_block);

    constexpr std::string_view two_comments{R"(const a: i32 = 2;

// first

// second
const b: i32 = 3;
)"};
    CHECK(format_source(two_comments) == two_comments);

    constexpr std::string_view no_blank{R"(const a: i32 = 2;
// some comment
const b: i32 = 3;
)"};
    CHECK(format_source(no_blank) == no_blank);

    CHECK(format_source("// file header\nconst a := 1;\n") == "// file header\nconst a := 1;\n");
}

constexpr std::string_view corpus{
#include "ast/golden.gh.inc"
};

TEST_CASE("formatter round trip: full node corpus") { round_trips(corpus); }

} // namespace ghoti::tests
