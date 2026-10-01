#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "compiler/sema/error.hh"

#include "helpers/codegen.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("const, let, let mut and comptime let mut each bind a value") {
    CHECK(helpers::compile_and_run(R"(
        const N = 16;
        let base: i32 = 10;
        let mut counter: i32 = 0;
        pub const main = fn(): i32 {
            let x = base + 2;
            let mut i: i32 = 0;
            while (i < 3) { i += 1; }
            counter += i;
            comptime let mut acc: i32 = 0;
            for comptime (0..4) |k| { acc += k; }
            return x + counter + acc + @intCast(i32, N);
        };
    )") == 10 + 2 + 3 + 6 + 16);
}

TEST_CASE("a let folds where a compile-time value is needed") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let n = 4;
            let arr: [n]u8 = .{ 1, 2, 3, 4 };
            return @intCast(i32, arr.len);
        };
    )") == 4);
}

TEST_CASE("compile-time-only values are bound with const") {
    CHECK(helpers::raised("let T = i32;", sema::error::MUTABLE_TYPE_BINDING));
    CHECK(helpers::raised("pub const main = fn(): i32 { let T = i32; return 0; };",
                          sema::error::MUTABLE_TYPE_BINDING));
    CHECK(helpers::raised("let U = undefined;", sema::error::COMPILE_TIME_ONLY_VALUE));
    CHECK(helpers::raised("let S = struct { x: i32 };", sema::error::ILLEGAL_NON_CONST_STATEMENT));
    CHECK(helpers::raised("let mut f = fn(): i32 { return 1; };",
                          sema::error::ILLEGAL_NON_CONST_STATEMENT));
    helpers::resolve_and_check("const T = i32; const U = undefined; const S = struct { x: T };");
}

TEST_CASE("a closure capturing runtime state is bound with let") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut x: i32 = 40;
            x += 1;
            let next = fn(): i32 { return x + 1; };
            return next();
        };
    )") == 42);
    CHECK(helpers::raised(R"(
        pub const main = fn(): i32 {
            let mut x: i32 = 40;
            const next = fn(): i32 { return x + 1; };
            return next();
        };
    )",
                          sema::error::ILLEGAL_BINDING_KIND));
    // A const closure may still capture another compile-time value
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            const base: i32 = 40;
            const next = fn(): i32 { return base + 2; };
            return next();
        };
    )") == 42);
}

TEST_CASE("linkage attaches to a const function and to let data") {
    llvm::LLVMContext context;
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        export("ghoti_counter") let mut counter: u64 = 0;
        export("ghoti_limit") let limit: u64 = 8;
        pub weak let mut hook_state: i32 = 0;
        pub weak const hook = fn(): i32 { return 1; };
        export("ghoti_bump") const bump = fn(): void { counter += 1; };
    )")};
    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK(llvm_mod->getNamedGlobal("ghoti_counter") != nullptr);
    CHECK(llvm_mod->getNamedGlobal("ghoti_limit") != nullptr);
    CHECK(llvm_mod->getFunction("ghoti_bump") != nullptr);
}

TEST_CASE("linkage on the wrong binding kind is an error") {
    CHECK(helpers::raised("export(\"x\") const x: i32 = 1;", sema::error::ILLEGAL_BINDING_KIND));
    CHECK(helpers::raised("weak const x: i32 = 1;", sema::error::ILLEGAL_BINDING_KIND));
    CHECK(helpers::raised("threadlocal const x: i32 = 1;", sema::error::ILLEGAL_BINDING_KIND));
    CHECK(helpers::raised("extern const x: i32;", sema::error::ILLEGAL_BINDING_KIND));
    CHECK(helpers::raised("extern let f: fn(): i32;", sema::error::ILLEGAL_BINDING_KIND));
    CHECK(helpers::raised("export(\"f\") let f = fn(): i32 { return 1; };",
                          sema::error::ILLEGAL_BINDING_KIND));
    helpers::resolve_and_check("extern let mut errno: i32; extern const puts: fn(s: ^u8): i32;");
}

TEST_CASE("the address of a const is a stable read-only value") {
    CHECK(helpers::compile_and_run(R"(
        const N: i32 = 7;
        const TABLE: [3]i32 = .{ 1, 2, 3 };
        pub const main = fn(): i32 {
            let p = ^N;
            let t = ^TABLE;
            return *p + (*t)[2];
        };
    )") == 10);
}

TEST_CASE("a const function infers compile-time params from its body") {
    CHECK(helpers::compile_and_run(R"(
        const min = fn(a: i32, b: i32): i32 { return if comptime (a < b) a else b; };
        const Of = struct {
            pub const size = fn(x: auto): usize {
                const s = @sizeOf(@TypeOf(x));
                return s;
            };
        };
        pub const main = fn(): i32 {
            const m = min(3, 9);
            let v: i64 = 5;
            return m + @intCast(i32, Of.size(v));
        };
    )") == 3 + 8);
    CHECK(helpers::raised(R"(
        const min = fn(a: i32, b: i32): i32 { return if comptime (a < b) a else b; };
        pub const main = fn(): i32 { let mut x: i32 = 1; return min(x, 4); };
    )",
                          sema::error::COMPTIME_EVALUATION_FAILED));
}

TEST_CASE("a let closure does not infer compile-time params") {
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            let pick = fn(a: i32, b: i32): i32 { return if comptime (a < b) a else b; };
            return pick(1, 2);
        };
    )");
}

} // namespace ghoti::tests
