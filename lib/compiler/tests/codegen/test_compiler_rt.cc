#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/TargetParser/Triple.h>
#include <stdx/result.hh>

#include "catch2/catch_message.hpp"
#include "compiler/codegen/error.hh"
#include "compiler/codegen/linker.hh"
#include "compiler/codegen/runtime_libcalls.hh"
#include "compiler/codegen/target.hh"
#include "compiler/runtime/compiler_rt.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/tempfile.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

// `f128` addition and comparison are runtime libcalls on every target
constexpr std::string_view f128_program{R"(
let mut a: f128 = 1.5;
let mut b: f128 = 2.25;

pub const main = fn(_: [][:0]u8): i32 {
    let c = a + b;
    if (@bitCast(u128, c) == @bitCast(u128, b)) { return 7; }
    if (@bitCast(u128, c) == @bitCast(u128, a)) { return 5; }
    return 3;
};
)"};

constexpr std::string_view plain_program{R"(
pub const main = fn(_: [][:0]u8): i32 { return 4; };
)"};

// Returns its second operand, so the caller can tell the value really crossed the ABI
constexpr std::string_view second_operand_rt{R"(
export const __addtf3 = fn(a: f128, b: f128): f128 {
    _ = a;
    return b;
};
)"};

constexpr std::string_view self_calling_rt{R"(
export const __addtf3 = fn(a: f128, b: f128): f128 {
    return a + b;
};
)"};

constexpr std::string_view broken_rt{R"(
export const __addtf3 = fn(a: f128, b: f128): f128 {
    return a + not_declared;
};
)"};

// A fresh compiler_rt root per test, so the per-process memo never serves another test's archive
struct fixture_root {
    tempdir               dir{"compiler_rt_fixture"};
    std::filesystem::path root{dir.path / "compiler_rt.gh"};

    explicit fixture_root(std::string_view source) { dir.write("compiler_rt.gh", source); }

    [[nodiscard]] auto linker_options() const -> codegen::extra_linker_options {
        codegen::extra_linker_options opts;
        opts.compiler_rt.emplace(codegen::compiler_rt_options{.root = root});
        return opts;
    }
};

[[nodiscard]] auto emit_program_object(std::string_view               source,
                                       const std::filesystem::path&   path,
                                       const codegen::target_options& target_opts)
    -> stdx::result<void, codegen::diagnostic> {
    auto              ctx_idx{helpers::type_check_and_verify(source, {})};
    llvm::LLVMContext context;
    return helpers::emit_object(*ctx_idx.first, context, path, target_opts);
}

[[nodiscard]] auto link_error(std::string_view                     source,
                              const codegen::target_options&       target_opts,
                              const codegen::extra_linker_options& linker_opts) -> std::string {
    auto              ctx_idx{helpers::type_check_and_verify(source, {})};
    llvm::LLVMContext context;
    const tempfile    exe{"compiler_rt_link_error"};
    const auto        emitted{
        helpers::emit_executable(*ctx_idx.first, context, exe.path, target_opts, {}, linker_opts)};
    REQUIRE_FALSE(emitted);
    return emitted.error().get_message().value_or("");
}

constexpr std::array tier_one_triples{
    "x86_64-unknown-linux-gnu",
    "aarch64-unknown-linux-gnu",
    "riscv64-unknown-linux-gnu",
    "x86_64-w64-windows-gnu",
    "x86_64-pc-windows-msvc",
    "x86_64-apple-macos",
    "arm64-apple-macos",
    "wasm32-unknown-unknown",
};

} // namespace

TEST_CASE("A compiler_rt routine resolves a runtime libcall end to end") {
    const fixture_root rt{second_operand_rt};
    CHECK(helpers::compile_and_run(f128_program, {}, rt.linker_options()) == 7);
}

TEST_CASE("compiler_rt builds once per target, however many programs link it") {
    const fixture_root rt{second_operand_rt};
    const auto         before{runtime::compiler_rt_builds()};
    CHECK(helpers::compile_and_run(f128_program, {}, rt.linker_options()) == 7);
    CHECK(helpers::compile_and_run(f128_program, {}, rt.linker_options()) == 7);
    CHECK(runtime::compiler_rt_builds() == before + 1);
}

TEST_CASE("compiler_rt isn't built for a program that calls no runtime libcall") {
    const fixture_root rt{broken_rt};
    const auto         before{runtime::compiler_rt_builds()};
    CHECK(helpers::compile_and_run(plain_program, {}, rt.linker_options()) == 4);
    CHECK(runtime::compiler_rt_builds() == before);
}

TEST_CASE("A compiler_rt that fails to compile is reported against its own file") {
    const fixture_root rt{broken_rt};
    for (const std::string_view triple : {"x86_64-unknown-linux-gnu", "x86_64-w64-windows-gnu"}) {
        const auto message{
            link_error(f128_program, {.triple_str = std::string{triple}}, rt.linker_options())};
        CHECK(message.contains(fmt::format("the compiler builtins failed to build for '{}'",
                                           codegen::resolve_target_triple(triple).str())));
        CHECK(message.contains("compiler_rt.gh"));
        CHECK(message.contains("not_declared"));
    }
}

TEST_CASE("A builtin compiled into a call to itself trips the recursion guard") {
    const fixture_root rt{self_calling_rt};
    for (const std::string_view triple : tier_one_triples) {
        INFO(triple);
        const auto message{
            link_error(f128_program, {.triple_str = std::string{triple}}, rt.linker_options())};
        CHECK(message.contains("call themselves"));
        CHECK(message.contains("`__addtf3` is compiled into a call to `__addtf3`"));
    }
}

TEST_CASE("The shipped compiler_rt builds for every tier-1 target") {
    for (const std::string_view triple : tier_one_triples) {
        INFO(triple);
        const codegen::target_options target_opts{.triple_str = std::string{triple}};
        const tempfile                object{"shipped_compiler_rt.o"};
        REQUIRE(emit_program_object(f128_program, object.path, target_opts));
        CHECK_FALSE(UNWRAP(runtime::compiler_rt_imports(object.path, target_opts)).empty());

        const auto archive{UNWRAP(runtime::resolve_compiler_rt(target_opts, object.path, {}))};
        REQUIRE(archive);
        CHECK(std::filesystem::exists(*archive));
    }
}

TEST_CASE("A compiler_rt that uses std builds for every tier-1 target") {
    // std builds even where it has no OS backend (wasm32, riscv64 Linux)
    const fixture_root rt{R"(
        import std;
        export const __addtf3 = fn(a: f128, b: f128): f128 {
            if (std.math.max(@as(i32, 1), @as(i32, 2)) == 2) { return b; }
            return a;
        };
    )"};
    for (const std::string_view triple : tier_one_triples) {
        INFO(triple);
        const codegen::target_options target_opts{.triple_str = std::string{triple}};
        const tempfile                object{"std_compiler_rt.o"};
        REQUIRE(emit_program_object(f128_program, object.path, target_opts));
        const auto archive{UNWRAP(runtime::resolve_compiler_rt(
            target_opts, object.path, codegen::compiler_rt_options{.root = rt.root}))};
        REQUIRE(archive);
        CHECK(std::filesystem::exists(*archive));
    }
}

TEST_CASE("compiler_rt imports are the runtime libcalls an object leaves undefined") {
    const codegen::target_options target_opts{.triple_str = "x86_64-unknown-linux-gnu"};
    const tempfile                object{"compiler_rt_imports.o"};
    REQUIRE(emit_program_object(f128_program, object.path, target_opts));
    const auto imports{UNWRAP(runtime::compiler_rt_imports(object.path, target_opts))};
    CHECK(std::ranges::contains(imports, "__addtf3"));
    // Globals and libc's memory routines are never compiler_rt's to provide
    CHECK_FALSE(std::ranges::contains(imports, "memcpy"));
    CHECK_FALSE(std::ranges::contains(imports, "a"));

    const tempfile plain{"compiler_rt_imports_plain.o"};
    REQUIRE(emit_program_object(plain_program, plain.path, target_opts));
    CHECK(UNWRAP(runtime::compiler_rt_imports(plain.path, target_opts)).empty());
}

TEST_CASE("Runtime libcall names follow the target") {
    const auto linux_triple{codegen::resolve_target_triple("x86_64-unknown-linux-gnu")};
    const auto msvc_triple{codegen::resolve_target_triple("x86_64-pc-windows-msvc")};
    const auto macos_triple{codegen::resolve_target_triple("arm64-apple-macos")};
    CHECK(codegen::is_runtime_libcall(linux_triple, "__addtf3"));
    CHECK(codegen::is_runtime_libcall(linux_triple, "fmodf"));
    CHECK_FALSE(codegen::is_runtime_libcall(linux_triple, "main"));
    CHECK_FALSE(codegen::is_compiler_rt_symbol(linux_triple, "memcpy"));
    CHECK(codegen::is_compiler_rt_symbol(msvc_triple, "_fltused"));
    CHECK_FALSE(codegen::is_compiler_rt_symbol(linux_triple, "_fltused"));
    CHECK(codegen::strip_global_prefix(macos_triple, "___addtf3") == "__addtf3");
    CHECK(codegen::strip_global_prefix(linux_triple, "__addtf3") == "__addtf3");
}

TEST_CASE("Self-calling builtins are found in any target's assembly") {
    const codegen::target_options linux_opts{.triple_str = "x86_64-unknown-linux-gnu"};
    constexpr std::string_view    self_call{"__addtf3:\n"
                                            "# %bb.0:\n"
                                            ".LBB0_1:\n"
                                            "\tjmp\t__addtf3@PLT\n"
                                            "\t.size\t__addtf3, .Lfunc_end0-__addtf3\n"};
    CHECK(runtime::find_self_calling_libcalls(self_call, linux_opts) ==
          std::vector<std::string>{"__addtf3"});

    // A call to a different builtin, or the function's own name in a directive, isn't recursion
    constexpr std::string_view other_call{"__multf3:\n"
                                          "\tcallq\t__addtf3@PLT\n"
                                          "\t.size\t__multf3, .Lfunc_end0-__multf3\n"
                                          "helper:\n"
                                          "\tcallq\thelper\n"};
    CHECK(runtime::find_self_calling_libcalls(other_call, linux_opts).empty());

    const codegen::target_options macos_opts{.triple_str = "arm64-apple-macos"};
    constexpr std::string_view    macho{"___addtf3:\n"
                                        "Ltmp0:\n"
                                        "\tb\t___addtf3\n"};
    CHECK(runtime::find_self_calling_libcalls(macho, macos_opts) ==
          std::vector<std::string>{"__addtf3"});
}

TEST_CASE("A link missing compiler builtins names them") {
    const fixture_root rt{"// defines nothing\n"};
    const auto         message{
        link_error(f128_program, {.triple_str = "x86_64-unknown-linux-gnu"}, rt.linker_options())};
    CHECK(message.contains("hint: `__addtf3` is a compiler builtin; lib/compiler_rt doesn't "
                           "provide it for 'x86_64-unknown-linux-gnu' yet"));

    const auto skipped{link_error(f128_program, {.triple_str = "x86_64-unknown-linux-gnu"}, {})};
    CHECK(skipped.contains("no builtins archive was linked"));
}

TEST_CASE("compiler_rt can use the standard library") {
    const fixture_root rt{R"(
        import std;
        export const __addtf3 = fn(a: f128, b: f128): f128 {
            let x: [2]u8 = .{ 1, 2 };
            let y: [2]u8 = .{ 1, 2 };
            if (std.mem.eql(u8, x[..], y[..])) { return b; }
            return a;
        };
    )"};
    CHECK(helpers::compile_and_run(f128_program, {}, rt.linker_options()) == 7);
}

TEST_CASE("compiler_rt's own functions stay internal to it") {
    // The builtins and the program both define a `pub const twice`; only exports leave the archive
    fixture_root rt{R"(
        import "helpers.gh" as helpers;
        export const __addtf3 = fn(a: f128, b: f128): f128 {
            _ = a;
            if (helpers.twice(2) == 4) { return b; }
            return a;
        };
    )"};
    rt.dir.write("helpers.gh", "pub const twice = fn(x: i32): i32 { return x * 2; };\n");
    CHECK(helpers::compile_and_run(R"(
        let mut a: f128 = 1.5;
        let mut b: f128 = 2.25;
        pub const twice = fn(x: i32): i32 { return x + x; };
        pub const main = fn(_: [][:0]u8): i32 {
            let c = a + b;
            if (@bitCast(u128, c) == @bitCast(u128, b)) { return twice(3); }
            return 3;
        };
    )",
                                   {},
                                   rt.linker_options()) == 6);
}

} // namespace ghoti::tests
