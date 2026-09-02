#include <algorithm>
#include <sstream>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "compiler/gir/dumper.hh"
#include "compiler/gir/emitter.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/sema/analyzer.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("GIR array indexing bounds checking") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const get_elem := fn(arr: [5]i32, i: usize): i32 {
            return arr[i];
        };

        const set_elem := fn(arr: [5]i32, i: usize, val: i32): void {
            arr[i] = val;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    // The two user functions plus the `weak` `panic_handler` pulled in for the bounds checks.
    REQUIRE(gir_mod.get_functions().size() == 3);
    bool saw_panic_handler{false};
    for (const auto& fn : gir_mod.get_functions()) {
        if (fn->get_name() == "panic_handler") {
            saw_panic_handler = true;
            continue;
        }

        std::ostringstream ss;
        gir::dumper        dumper{ss};
        dumper.dump(*fn);
        const auto dump_text{ss.view()};

        CHECK(dump_text.contains("lt bool"));
        CHECK(dump_text.contains("cond_goto"));
        CHECK(dump_text.contains("call @panic_handler"));
        CHECK(dump_text.contains("unreachable"));
        CHECK(dump_text.contains("get_element_ptr"));
    }
    CHECK(saw_panic_handler);
}

TEST_CASE("GIR @panic lowers to an overridable panic_handler call") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const boom := fn(): void {
            @panic("kaboom");
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 2);

    const gir::function* boom{nullptr};
    bool                 saw_panic_handler{false};
    for (const auto& fn : gir_mod.get_functions()) {
        if (fn->get_name() == "boom") { boom = fn; }
        if (fn->get_name() == "panic_handler") { saw_panic_handler = true; }
    }
    REQUIRE(boom);
    CHECK(saw_panic_handler);

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(*boom);
    const auto dump_text{ss.view()};

    CHECK(dump_text.contains("call @panic_handler"));
    CHECK(dump_text.contains("\"kaboom\""));
    CHECK(dump_text.contains("unreachable"));
}

TEST_CASE("GIR a reached `unreachable` routes through panic_handler") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const pick := fn(b: bool): i32 {
            if (b) { return 1; }
            unreachable;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    const gir::function* pick{nullptr};
    for (const auto& fn : gir_mod.get_functions()) {
        if (fn->get_name() == "pick") { pick = fn; }
    }
    REQUIRE(pick);

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(*pick);
    CHECK(ss.view().contains("call @panic_handler"));
}

TEST_CASE("GIR array literal stack allocation and initialization") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const test_array_lit := fn(): i32 {
            var a: [3]i32 = [3]i32{ 10, 20, 30 };
            return a[1];
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    // `test_array_lit` plus the `weak` `panic_handler` pulled in for the `a[1]` bounds check.
    REQUIRE(gir_mod.get_functions().size() == 2);
    const auto& fn{*gir_mod.get_functions()[0]};

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(fn);
    const auto dump_text{ss.view()};

    CHECK(dump_text.contains("alloca"));
    CHECK(dump_text.contains("get_element_ptr"));
}

TEST_CASE("GIR extern decls default to the \"c\" ABI target") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        extern const puts: fn(s: ^u8): i32;
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 1);
    CHECK(gir_mod.get_functions()[0]->get_abi_name() == "c");
    CHECK(gir_mod.get_required_libraries().empty());
}

TEST_CASE("GIR extern decls record an explicit ABI target") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        extern("kernel32") const GetLastError: fn(): void;
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 1);
    CHECK(gir_mod.get_functions()[0]->get_abi_name() == "kernel32");

    const auto libraries{gir_mod.get_required_libraries()};
    REQUIRE(libraries.size() == 1);
    CHECK(libraries[0] == "kernel32");
}

TEST_CASE("GIR required libraries are deduplicated") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        extern("kernel32") const GetLastError: fn(): void;
        extern("kernel32") const GetCurrentProcessId: fn(): void;
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    const auto libraries{gir_mod.get_required_libraries()};
    REQUIRE(libraries.size() == 1);
    CHECK(libraries[0] == "kernel32");
}

TEST_CASE("GIR builtins cast operations") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const test_casts := fn(x: i32, ptr: ^i32): usize {
            const w := @as(i64, x);
            const u := @intFromPtr(ptr);
            return u;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 1);
    const auto& fn{*gir_mod.get_functions()[0]};

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(fn);
    const auto dump_text{ss.view()};

    CHECK(dump_text.contains("widen_cast"));
    CHECK(dump_text.contains("int_from_ptr"));
}

TEST_CASE("GIR emit_gir integration in pipeline") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const compute := fn(a: i32, b: i32): i32 {
            return a * b + 1;
        };
    )")};

    const auto gir_mod{ctx->analyzer.emit_gir(ctx->root_mod)};
    REQUIRE(gir_mod.get_functions().size() == 1);
    const auto& fn{*gir_mod.get_functions()[0]};
    CHECK(fn.get_name() == "compute");

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(fn);
    const auto dump_text{ss.view()};

    CHECK(dump_text.contains("mul i32"));
    CHECK(dump_text.contains("add i32"));
    CHECK(dump_text.contains("ret i32"));
}

TEST_CASE("GIR linkage and visibility attributes") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        export const exp_fn := fn(): void {};
        pub const pub_fn := fn(): void {};
        const priv_fn := fn(): void {};
        export const exp_global: i32 = 10;
        pub const pub_global: i32 = 20;
        const priv_global: i32 = 30;
    )")};

    const auto gir_mod{ctx->analyzer.emit_gir(ctx->root_mod)};
    for (const auto* fn : gir_mod.get_functions()) {
        if (fn->get_name() == "exp_fn") {
            CHECK(fn->get_linkage() == gir::linkage::EXPORT);
        } else if (fn->get_name() == "pub_fn") {
            CHECK(fn->get_linkage() == gir::linkage::PUBLIC);
        } else if (fn->get_name() == "priv_fn") {
            CHECK(fn->get_linkage() == gir::linkage::INTERNAL);
        }
    }

    for (const auto* glob : gir_mod.get_globals()) {
        if (glob->name == "exp_global") {
            CHECK(glob->linkage == gir::linkage::EXPORT);
        } else if (glob->name == "pub_global") {
            CHECK(glob->linkage == gir::linkage::PUBLIC);
        } else if (glob->name == "priv_global") {
            CHECK(glob->linkage == gir::linkage::INTERNAL);
        }
    }
}

TEST_CASE("GIR slice indexing and fat pointer operations") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const slice_ops := fn(s: []i32, i: usize): i32 {
            return s[i];
        };
    )")};

    const auto gir_mod{ctx->analyzer.emit_gir(ctx->root_mod)};
    // `slice_ops` plus the `weak` `panic_handler` pulled in for the `s[i]` bounds check.
    REQUIRE(gir_mod.get_functions().size() == 2);
    const auto& fn{*gir_mod.get_functions()[0]};

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(fn);
    const auto dump_text{ss.view()};

    // Fat pointer field extraction (ptr at 0, len at 1), bounds check, and load
    CHECK(dump_text.contains("get_element_ptr"));
    CHECK(dump_text.contains("lt bool"));
    CHECK(dump_text.contains("cond_goto"));
    CHECK(dump_text.contains("unreachable"));
    CHECK(dump_text.contains("load"));
}

TEST_CASE("GIR indirect call formatting") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const target := fn(x: i32): i32 {
            return x + 1;
        };
        const call_indirect := fn(): i32 {
            var fptr: ^fn(n: i32): i32 = target;
            return fptr(42);
        };
    )")};

    const auto gir_mod{ctx->analyzer.emit_gir(ctx->root_mod)};
    const auto fn_it{std::ranges::find_if(
        gir_mod.get_functions(), [](const auto* f) { return f->get_name() == "call_indirect"; })};
    REQUIRE(fn_it != gir_mod.get_functions().end());

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(**fn_it);
    const auto dump_text{ss.view()};

    CHECK(dump_text.contains("call %"));
}

} // namespace ghoti::tests
