#include <set>
#include <sstream>
#include <string>
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
            CHECK(sema::type_kind_display_name(
                      fn->get_type().get_data().as<sema::types::function>().return_type) == "i32");
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

TEST_CASE("GIR constexpr parameter monomorphizes per value") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const shifted := fn(constexpr by: i32, x: i32): i32 {
            return x + by;
        };

        const test_fn := fn(): i32 {
            return shifted(10, 1) + shifted(100, 2) + shifted(10, 3);
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    std::set<std::string_view> shifted_variants;
    for (const auto& fn : gir_mod.get_functions()) {
        if (fn->get_name().starts_with("shifted__")) {
            shifted_variants.insert(fn->get_name());
            // `constexpr by` is erased from the signature; only the runtime `x` remains.
            REQUIRE(fn->get_params().size() == 1);
            CHECK(fn->get_params()[0]->name == "x");
        }
    }
    CHECK(shifted_variants.size() == 2);
}

TEST_CASE("GIR constexpr parameter sizes a type per instantiation") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const room := fn(constexpr n: usize): usize { return @sizeOf([n]i32); };

        const test_fn := fn(): usize {
            return room(2uz) + room(5uz);
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    std::set<std::string> rets;
    for (const auto& fn : gir_mod.get_functions()) {
        if (!fn->get_name().starts_with("room__")) { continue; }
        std::ostringstream ss;
        gir::dumper        dumper{ss};
        dumper.dump(*fn);
        rets.insert(std::string{ss.view()});
    }
    // Two instantiations with different bodies: [2]i32 -> 8, [5]i32 -> 20.
    REQUIRE(rets.size() == 2);
}

TEST_CASE("GIR constexpr struct value dedups regardless of field order") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const P := struct { x: i32, y: i32 };
        const dot := fn(constexpr p: P): i32 { return p.x + p.y; };

        const test_fn := fn(): i32 {
            return dot(P{ .x = 1, .y = 2 }) + dot(P{ .y = 2, .x = 1 }) + dot(P{ .x = 9, .y = 9 });
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    std::set<std::string_view> dot_variants;
    for (const auto& fn : gir_mod.get_functions()) {
        if (fn->get_name().starts_with("dot__")) { dot_variants.insert(fn->get_name()); }
    }
    CHECK(dot_variants.size() == 2);
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
            CHECK(sema::type_kind_display_name(
                      fn->get_type().get_data().as<sema::types::function>().return_type) == "i32");
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

} // namespace ghoti::tests
