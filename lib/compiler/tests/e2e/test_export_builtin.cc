#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "helpers/codegen.hh"
#include "helpers/common.hh"
#include "helpers/formatter.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

// compiler_rt's shape: one body, several weak hidden names, picked per target
constexpr std::string_view RUNTIME_STYLE{R"(
    const add := fn(a: i32, b: i32): i32 { return a + b; };
    @export(add, .{ .name = "__ghoti_add", .linkage = .weak, .visibility = .hidden });
    @export(add, .{ .name = "ghoti_add_alias" });
    pub const use := fn(): i32 { return add(1, 2); };
)"};

} // namespace

TEST_CASE("@export defines a function under another symbol, with linkage and visibility") {
    llvm::LLVMContext context;
    auto [ctx, idx]{helpers::resolve_and_check(RUNTIME_STYLE)};
    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};

    const auto* primary{llvm_mod->getFunction("__ghoti_add")};
    REQUIRE(primary != nullptr);
    CHECK(primary->hasWeakAnyLinkage());
    CHECK(primary->hasHiddenVisibility());

    // A second export is another symbol for the same code
    const auto* alias{llvm_mod->getNamedAlias("ghoti_add_alias")};
    REQUIRE(alias != nullptr);
    CHECK(alias->getAliasee() == primary);
    CHECK(alias->hasExternalLinkage());
    CHECK(alias->hasDefaultVisibility());
}

TEST_CASE("@export works on every object format") {
    const auto        triple{GENERATE("x86_64-unknown-linux-gnu",
                               "x86_64-pc-windows-msvc",
                               "aarch64-apple-macos",
                               "wasm32-unknown-unknown")};
    llvm::LLVMContext context;
    auto [ctx, idx]{helpers::type_check_for_target(RUNTIME_STYLE, triple)};
    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir(*ctx, context))};
    CHECK(llvm_mod->getFunction("__ghoti_add") != nullptr);
    CHECK(llvm_mod->getNamedAlias("ghoti_add_alias") != nullptr);
}

TEST_CASE("an exported function in an imported module survives pruning") {
    llvm::LLVMContext context;
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        import "rt.gh" as rt;
        pub const main := fn(): i32 { return 0; };
    )",
                                               {{"rt.gh", R"(
        const twice := fn(x: i32): i32 { return x * 2; };
        @export(twice, .{ .name = "rt_twice", .linkage = .weak });
        export("rt_triple") const triple := fn(x: i32): i32 { return x * 3; };
    )"}})};
    auto llvm_mod{UNWRAP(helpers::emit_llvm_ir_executable(*ctx, context))};
    CHECK(llvm_mod->getFunction("rt_twice") != nullptr);
    CHECK(llvm_mod->getFunction("rt_triple") != nullptr);
}

TEST_CASE("a helper like Zig's `symbol` exports whatever function it's handed") {
    llvm::LLVMContext context;
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        import "add.gh" as add;
        pub const main := fn(): i32 { return 0; };
    )",
                                               {{"common.gh", R"(
        pub const symbol := fn(comptime f: auto, comptime name: [:0]u8): void {
            @export(f, .{ .name = name, .linkage = .weak, .visibility = .hidden });
        };
    )"},
                                                {"add.gh", R"(
        import "common.gh" as common;
        const addtf3 := fn(a: i32, b: i32): i32 { return a + b; };
        common.symbol(addtf3, "__ghoti_addtf3");
        common.symbol(addtf3, "__aeabi_ghoti_addtf3");
    )"}})};
    auto        llvm_mod{UNWRAP(helpers::emit_llvm_ir_executable(*ctx, context))};
    const auto* primary{llvm_mod->getFunction("__ghoti_addtf3")};
    REQUIRE(primary != nullptr);
    CHECK(primary->hasWeakAnyLinkage());
    CHECK(primary->hasHiddenVisibility());
    const auto* alias{llvm_mod->getNamedAlias("__aeabi_ghoti_addtf3")};
    REQUIRE(alias != nullptr);
    CHECK(alias->hasWeakAnyLinkage());
    CHECK(alias->hasHiddenVisibility());
}

TEST_CASE("a comptime function argument from another module calls the right function") {
    // The callee's module has its own `pick`; the argument must still mean the caller's
    CHECK(helpers::compile_and_run(R"(
        import "apply.gh" as apply;
        const pick := fn(): i32 { return 42; };
        pub const main := fn(): i32 { return apply.call(pick); };
    )",
                                   {{"apply.gh", R"(
        const pick := fn(): i32 { return 1; };
        pub const call := fn(comptime f: auto): i32 { return f(); };
    )"}}) == 42);
}

TEST_CASE("@export as 'main' makes a function the entry point") {
    CHECK(helpers::compile_and_run(R"(
        const add := fn(a: i32, b: i32): i32 { return a + b; };
        const entry := fn(): i32 { return add(40, 2); };
        @export(entry, .{ .name = "main" });
    )") == 42);
    // Exported alongside other names it still runs
    CHECK(helpers::compile_and_run(R"(
        const entry := fn(): i32 { return 7; };
        @export(entry, .{ .name = "main" });
        @export(entry, .{ .name = "also_entry" });
    )") == 7);
}

TEST_CASE("@export reports what it can't do") {
    helpers::expect_compile_error(R"(
        const id := fn(x: auto): @TypeOf(x) { return x; };
        @export(id, .{ .name = "id" });
    )");
    // One name for two functions (the same export written twice is harmless)
    helpers::expect_compile_error(R"(
        const f := fn(): i32 { return 1; };
        const g := fn(): i32 { return 2; };
        @export(f, .{ .name = "x" });
        @export(g, .{ .name = "x" });
    )");
    helpers::expect_compile_error(R"(
        export("f") const f := fn(): i32 { return 1; };
        @export(f, .{ .name = "g" });
    )");
    helpers::expect_compile_error(R"(
        const f := fn(): i32 { return 1; };
        @export(f, 3);
    )");
    // Two entry points
    helpers::expect_compile_error(R"(
        const f := fn(): i32 { return 1; };
        @export(f, .{ .name = "main" });
        pub const main := fn(): i32 { return 0; };
    )");
    // `.protected` is ELF-only
    helpers::expect_compile_error(R"(
        const f := fn(): i32 { return 1; };
        @export(f, .{ .name = "p", .visibility = .protected });
    )");
}

TEST_CASE("a keyword is a name right after `.`") {
    CHECK(helpers::compile_and_run(R"(
        const Options := struct { @"weak": bool, @"type": i32 };
        const Mode := enum { strong, @"weak" };
        pub const main := fn(): i32 {
            let o := Options{ .weak = true, .type = 40 };
            let m: Mode = .weak;
            if (!o.weak or m != Mode.weak) { return 1; }
            return o.type + 2;
        };
    )") == 42);
    // The formatter keeps the plain spelling after `.`, and the raw one in the declaration
    const auto source{"const m := .{ .weak = x.type };\n"};
    CHECK(helpers::format_source(source) == source);
    helpers::round_trips("const E := enum { @\"weak\" };\nconst w: E = .@\"weak\";\n");
    CHECK(helpers::format_source("const w: E = .@\"weak\";") == "const w: E = .weak;\n");
}

} // namespace ghoti::tests
