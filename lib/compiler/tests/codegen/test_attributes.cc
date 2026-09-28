#include <algorithm>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
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
    CHECK(std::ranges::none_of(always.users(), [](const llvm::User* user) {
        return llvm::isa<llvm::CallBase>(user);
    }));
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

TEST_CASE("The removed `naked fn` prefix no longer parses") {
    syntax::parser p{"const f := naked fn(): void {};"};
    ghoti::arena   arena;
    ast::AST       ast;
    CHECK_FALSE(p.consume(ast, arena).empty());
}

} // namespace ghoti::tests
