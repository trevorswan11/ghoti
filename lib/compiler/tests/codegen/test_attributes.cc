#include <algorithm>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>

#include "compiler/arena.hh"
#include "compiler/ast/ast.hh"
#include "compiler/sema/error.hh"
#include "compiler/syntax/parser.hh"
#include "helpers/codegen.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] auto has_error(std::string_view source, sema::error code) -> bool {
    const auto [codes, _]{helpers::resolve_diags(source)};
    return std::ranges::contains(codes, code);
}

} // namespace

TEST_CASE("Codegen: @[inline(...)] maps each mode onto its LLVM attribute") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        @[inline(.always)] pub const always := fn(x: i32): i32 { return x; };
        @[inline(.never)] pub const never := fn(x: i32): i32 { return x; };
        pub const hint := @[inline(.hint)] fn(x: i32): i32 { return x; };
        constexpr FAST := true;
        @[inline(if (FAST) .always else .never)] pub const computed := fn(x: i32): i32 {
            return x;
        };
        pub const plain := fn(x: i32): i32 { return x; };
        pub const main := fn(args: [][:0]u8): i32 {
            return always(1) + never(2) + hint(3) + computed(4) + plain(5);
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));

    auto& always{UNWRAP(llvm_mod->getFunction("always"))};
    CHECK(always.hasFnAttribute(llvm::Attribute::AlwaysInline));
    // The always-inliner runs even at -O0, so no call to `always` survives
    CHECK(std::ranges::none_of(
        always.users(), [](const llvm::User* user) { return llvm::isa<llvm::CallBase>(user); }));
    CHECK(UNWRAP(llvm_mod->getFunction("never")).hasFnAttribute(llvm::Attribute::NoInline));
    CHECK(UNWRAP(llvm_mod->getFunction("hint")).hasFnAttribute(llvm::Attribute::InlineHint));
    CHECK(UNWRAP(llvm_mod->getFunction("computed")).hasFnAttribute(llvm::Attribute::AlwaysInline));

    auto& plain{UNWRAP(llvm_mod->getFunction("plain"))};
    CHECK_FALSE(plain.hasFnAttribute(llvm::Attribute::AlwaysInline));
    CHECK_FALSE(plain.hasFnAttribute(llvm::Attribute::NoInline));
    CHECK_FALSE(plain.hasFnAttribute(llvm::Attribute::InlineHint));
}

TEST_CASE("Codegen: @[naked] on a function literal is naked and never inlined") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        pub const trap_stub := @[naked] fn(): void {
            asm {
                template: "",
                options: (volatile, noreturn),
            };
        };
        pub const main := fn(args: [][:0]u8): void {};
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));
    auto& fn{UNWRAP(llvm_mod->getFunction("trap_stub"))};
    CHECK(fn.hasFnAttribute(llvm::Attribute::Naked));
    CHECK(fn.hasFnAttribute(llvm::Attribute::NoInline));
}

TEST_CASE("Function-only attributes are validated") {
    SECTION("inline needs a builtin.Inline value") {
        CHECK(has_error("@[inline(1)] const f := fn(): i32 { return 0; };",
                        sema::error::ILLEGAL_ATTRIBUTE));
        CHECK(has_error("@[inline(.sometimes)] const f := fn(): i32 { return 0; };",
                        sema::error::ILLEGAL_ATTRIBUTE));
    }
    SECTION("naked cannot also be inline(.always)") {
        CHECK(has_error("@[naked, inline(.always)] const f := fn(): void {};",
                        sema::error::ILLEGAL_ATTRIBUTE));
    }
    SECTION("a declaration that is not initialized by a function literal") {
        CHECK(has_error(R"(
            const f := fn(): i32 { return 0; };
            @[inline(.always)] const g := f;
        )",
                        sema::error::ILLEGAL_ATTRIBUTE));
        CHECK(has_error("@[naked] extern const f: fn(): void;", sema::error::ILLEGAL_ATTRIBUTE));
        CHECK(has_error("@[inline(.never)] const X: i32 = 3;", sema::error::ILLEGAL_ATTRIBUTE));
    }
    SECTION("the same attribute on the declaration and its literal") {
        CHECK(has_error("@[inline(.never)] const f := @[inline(.always)] fn(): i32 { return 0; };",
                        sema::error::ILLEGAL_ATTRIBUTE));
    }
}

TEST_CASE("@[align(n)] raises a field's alignment in the aggregate layout") {
    helpers::resolve_and_check(R"(
        const S := struct { a: u8, @[align(16)] b: u8 };
        const Outer := struct { tag: u8, inner: S };
        constexpr {
            @assert(@alignOf(S) == 16);
            @assert(@sizeOf(S) == 32);
            @assert(@sizeOf(Outer) == 48);
        }
    )");
}

TEST_CASE("Codegen: an over-aligned field is laid out at its aligned offset") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const S := struct { a: u8, @[align(16)] b: u8, c: u8 };
        pub var global_s: S = .{ .a = 1, .b = 2, .c = 3 };
        pub const main := fn(args: [][:0]u8): i32 {
            var s: S = .{ .a = 4, .b = 5, .c = 6 };
            s.b += global_s.b;
            const bp: ^mut u8 = ^mut s.b;
            const p: ^mut S = @fieldParentPtr(S, "b", bp);
            return @as(i32, s.b) + @as(i32, p.c);
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));

    auto&       global_s{UNWRAP(llvm_mod->getGlobalVariable("global_s"))};
    const auto& dl{llvm_mod->getDataLayout()};
    auto*       struct_ty{llvm::cast<llvm::StructType>(global_s.getValueType())};
    CHECK(dl.getTypeAllocSize(struct_ty) == 32);
    CHECK(global_s.getAlign() == llvm::MaybeAlign{16});
    // `b` sits after 15 bytes of padding, not at LLVM's natural offset 1
    CHECK(dl.getStructLayout(struct_ty)->getElementOffset(2) == 16);
}

TEST_CASE("@[align(n)] is not supported on union fields") {
    CHECK(has_error("const U := union { @[align(8)] a: u8, b: u16 };",
                    sema::error::ILLEGAL_ATTRIBUTE));
}

TEST_CASE("Codegen: @[align(n)] on globals, locals and functions") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        @[align(64)] pub var buffer: [4]u8 = undefined;
        @[align(32)] pub const aligned_fn := fn(x: i32): i32 { return x; };
        pub const main := fn(args: [][:0]u8): i32 {
            @[align(32)] var counter: i32 = 0;
            @[align(16)] const fixed: i32 = 5;
            counter += fixed;
            return counter + aligned_fn(1) + buffer[0];
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));

    CHECK(UNWRAP(llvm_mod->getGlobalVariable("buffer")).getAlign() == llvm::MaybeAlign{64});
    CHECK(UNWRAP(llvm_mod->getFunction("aligned_fn")).getAlign() == llvm::MaybeAlign{32});

    usize aligned_slots{0};
    for (auto& fn : *llvm_mod) {
        for (auto& bb : fn) {
            for (auto& inst : bb) {
                if (const auto* slot{llvm::dyn_cast<llvm::AllocaInst>(&inst)}) {
                    if (slot->getAlign().value() >= 16) { ++aligned_slots; }
                }
            }
        }
    }
    CHECK(aligned_slots == 2);
}

TEST_CASE("@[align(n)] is validated") {
    CHECK(has_error("@[align(3)] var x: i32 = 0;", sema::error::ILLEGAL_ATTRIBUTE));
    CHECK(has_error("@[align(0)] var x: i32 = 0;", sema::error::ILLEGAL_ATTRIBUTE));
    CHECK(has_error("@[align(8)] constexpr X := 3;", sema::error::ILLEGAL_ATTRIBUTE));
    CHECK(has_error("const S := struct { @[inline(.always)] a: i32 };",
                    sema::error::ILLEGAL_ATTRIBUTE));
    CHECK(
        has_error("const S := struct { @[discardable] a: i32 };", sema::error::ILLEGAL_ATTRIBUTE));
}

TEST_CASE("An attribute list inside an aggregate starts a field or a member") {
    helpers::resolve_and_check(R"(
        const S := struct {
            @[align(8)] a: u8,
            @[align(4)] pub b: u8,
            @[discardable] pub const get := fn(&self): u8 { return self.a; };
        };
        pub const main := fn(): i32 {
            const s: S = .{ .a = 1, .b = 2 };
            s.get();
            return 0;
        };
    )");
}

TEST_CASE("The removed `@alignas` builtin no longer parses") {
    syntax::parser p{"const S := struct { @alignas(4) a: i32 };"};
    ghoti::arena   arena;
    ast::AST       ast;
    CHECK_FALSE(p.consume(ast, arena).empty());
}

TEST_CASE("Codegen: a generic function's attributes fold per instantiation") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        @[align(if (@sizeOf(T) > 4) 64 else 16)]
        const first := fn(T: type, a: T, b: T): T { return a; };
        pub const main := fn(args: [][:0]u8): i32 {
            const small := first(i32, 1, 2);
            const big := first(i64, 3, 4);
            return small + @intCast(i32, big);
        };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));

    std::vector<u64> alignments;
    for (const auto& fn : *llvm_mod) {
        if (fn.getName().starts_with("first") && !fn.isDeclaration()) {
            alignments.emplace_back(fn.getAlign().valueOrOne().value());
        }
    }
    std::ranges::sort(alignments);
    CHECK(alignments == std::vector<u64>{16, 64});
}

TEST_CASE("A generic function's conditional discardable folds per instantiation") {
    constexpr std::string_view generic{R"(
        @[discardable(@sizeOf(T) == 1)]
        const echo := fn(T: type, x: T): T { return x; };
    )"};
    helpers::resolve_and_check(
        fmt::format("{}\npub const main := fn(): i32 {{ echo(u8, 1); return 0; }};", generic));
    CHECK(has_error(
        fmt::format("{}\npub const main := fn(): i32 {{ echo(i32, 1); return 0; }};", generic),
        sema::error::UNUSED_RESULT));
}

TEST_CASE("A constexpr parameter reaches the function's attribute arguments") {
    llvm::LLVMContext context;

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        @[align(if (n > 2) 64 else 32)]
        const scaled := fn(constexpr n: i32, x: i32): i32 { return x * n; };
        pub const main := fn(args: [][:0]u8): i32 { return scaled(2, 1) + scaled(3, 1); };
    )")};

    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK_FALSE(llvm::verifyModule(*llvm_mod));
    std::vector<u64> alignments;
    for (const auto& fn : *llvm_mod) {
        if (fn.getName().starts_with("scaled") && !fn.isDeclaration()) {
            alignments.emplace_back(fn.getAlign().valueOrOne().value());
        }
    }
    std::ranges::sort(alignments);
    CHECK(alignments == std::vector<u64>{32, 64});
}

TEST_CASE("A type constructor's field attributes fold per instantiation") {
    helpers::resolve_and_check(R"(
        const Boxed := fn(T: type): type {
            return struct { tag: u8, @[align(@alignOf(T) * 4)] value: T };
        };
        constexpr {
            @assert(@alignOf(Boxed(i32)) == 16);
            @assert(@alignOf(Boxed(u8)) == 4);
            @assert(@sizeOf(Boxed(i32)) == 32);
        }
    )");
}

TEST_CASE("An attribute argument must be known at compile time") {
    CHECK(has_error("@[align(n)] const f := fn(n: usize): i32 { return 0; };",
                    sema::error::ILLEGAL_ATTRIBUTE));
}

TEST_CASE("The removed `naked fn` prefix no longer parses") {
    syntax::parser p{"const f := naked fn(): void {};"};
    ghoti::arena   arena;
    ast::AST       ast;
    CHECK_FALSE(p.consume(ast, arena).empty());
}

} // namespace ghoti::tests
