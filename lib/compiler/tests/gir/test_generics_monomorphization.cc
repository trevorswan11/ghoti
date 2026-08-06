#include <algorithm>
#include <sstream>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "compiler/gir/dumper.hh"
#include "compiler/gir/emitter.hh"
#include "compiler/sema/type.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("GIR single monomorphized instantiation") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const identity := fn(x: auto): auto {
            return x;
        };

        const test_fn := fn(): i32 {
            return identity(42);
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 2);
    bool has_identity_i32{false}, has_test_fn{false};
    for (const auto& fn : gir_mod.get_functions()) {
        if (fn->get_name() == "identity__i32") {
            has_identity_i32 = true;
            CHECK(fn->get_params().size() == 1);
            CHECK(fn->get_type().get_data().as<sema::types::function>().return_type.get_kind() ==
                  sema::type_kind::I32);
        } else if (fn->get_name() == "test_fn") {
            has_test_fn = true;
            std::ostringstream ss;
            gir::dumper        dumper{ss};
            dumper.dump(*fn);
            const auto dump_text{ss.str()};
            CHECK(dump_text.contains("call @identity__i32"));
        }
    }

    CHECK(has_identity_i32);
    CHECK(has_test_fn);
}

TEST_CASE("GIR multiple instantiations with diverse types") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const add := fn(a: auto, b: auto): auto {
            return a + b;
        };

        const test_multi := fn(x: i32, y: i32, u: f64, v: f64): void {
            const r1 := add(x, y);
            const r2 := add(u, v);
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 3);
    bool has_add_i32{false}, has_add_f64{false}, has_test_multi{false};
    for (const auto& fn : gir_mod.get_functions()) {
        if (fn->get_name() == "add__i32_i32") {
            has_add_i32 = true;
            CHECK(fn->get_params().size() == 2);
            CHECK(fn->get_type().get_data().as<sema::types::function>().return_type.get_kind() ==
                  sema::type_kind::I32);
        } else if (fn->get_name() == "add__f64_f64") {
            has_add_f64 = true;
            CHECK(fn->get_params().size() == 2);
            CHECK(fn->get_type().get_data().as<sema::types::function>().return_type.get_kind() ==
                  sema::type_kind::F64);
        } else if (fn->get_name() == "test_multi") {
            has_test_multi = true;
            std::ostringstream ss;
            gir::dumper        dumper{ss};
            dumper.dump(*fn);
            const auto dump_text{ss.str()};
            CHECK(dump_text.contains("call @add__i32_i32"));
            CHECK(dump_text.contains("call @add__f64_f64"));
        }
    }

    CHECK(has_add_i32);
    CHECK(has_add_f64);
    CHECK(has_test_multi);
}

TEST_CASE("GIR monomorphization with slice parameter and array argument") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const map := fn(T: type, arr: []T, Ctx: type, func: fn(T, Ctx): T, ctx: Ctx): void {
            var i: usize = 0;
            for (arr) |&v| {
                v = func(arr[i], ctx);
            }

            var j: &T = undefined;
            while (i < arr.len) : (i += 1uz) {
                j = &arr[i];
                arr[i] = func(arr[i], ctx);
            };
        };

        const MyCtx := struct { offset: i32 };

        const test_map := fn(): void {
            const slice := [6]i32{1, 2, 3, 4, 5, 6};
            map(i32, slice, MyCtx, fn(x: i32, ctx: MyCtx): i32 {
                return x + ctx.offset;
            }, MyCtx{ .offset = 10 });
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    ;
    CHECK(std::ranges::any_of(gir_mod.get_functions(),
                              [](const auto& fn) { return fn->get_name().starts_with("map"); }));
}

} // namespace ghoti::tests
