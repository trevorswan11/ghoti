#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <stdx/option.hh>

#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/gir/segment.hh"
#include "compiler/gir/verifier.hh"

#include "helpers/uir_diff.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view PROGRAM{R"(
    pub const add = fn(a: i32, b: i32): i32 { return a + b; };
    pub const pick = fn(c: bool, x: i32): i32 {
        if (c) { return x; }
        return add(x, 2);
    };
)"};

[[nodiscard]] auto function_named(gir::module_t& mod, std::string_view name) -> gir::function& {
    for (const auto fn : mod.get_functions()) {
        if (fn->get_name() == name) { return *fn; }
    }
    FAIL("no function '" << name << "'");
    std::unreachable();
}

// The first instruction of `kind` in `fn`, with the segment holding it
struct found {
    gir::segment&     seg;
    gir::instruction& inst;
};

[[nodiscard]] auto first_of(gir::function& fn, gir::instruction_kind kind) -> found {
    for (const auto seg : fn.get_segments()) {
        for (const auto inst : seg->get_instructions()) {
            if (inst->kind == kind) { return found{.seg = *seg, .inst = *inst}; }
        }
    }
    FAIL("no " << gir::instruction_kind_name(kind) << " in '" << fn.get_name() << "'");
    std::unreachable();
}

[[nodiscard]] auto reported(const gir::module_t& mod, std::string_view text) -> bool {
    const auto report{gir::format_violations(gir::verify(mod))};
    INFO(report);
    return report.contains(text);
}

} // namespace

TEST_CASE("The GIR verifier accepts what UIR Sema builds") {
    const auto run{helpers::run_uir_sema(PROGRAM)};
    REQUIRE(run->out->succeeded());
    const auto found{gir::verify(*run->out->gir)};
    INFO(gir::format_violations(found));
    CHECK(found.empty());
}

TEST_CASE("The GIR verifier reports broken control flow") {
    auto run{helpers::run_uir_sema(PROGRAM)};
    REQUIRE(run->out->succeeded());
    auto& mod{*run->out->gir};

    SECTION("a segment without a terminator") {
        auto [seg, ret]{first_of(function_named(mod, "add"), gir::instruction_kind::RET)};
        seg.get_instructions().pop_back();
        CHECK(reported(mod, "has no terminator"));
    }

    SECTION("a terminator before the end of its segment") {
        auto [seg, ret]{first_of(function_named(mod, "add"), gir::instruction_kind::RET)};
        seg.append(gir::instruction{.kind = gir::instruction_kind::UNREACHABLE});
        CHECK(reported(mod, "a terminator before the end of its segment"));
    }

    SECTION("a branch to a segment that doesn't exist") {
        auto [seg, branch]{first_of(function_named(mod, "pick"), gir::instruction_kind::COND_GOTO)};
        branch.true_segment = gir::segment_id{99};
        CHECK(reported(mod, "branches to a segment that doesn't exist"));
    }

    SECTION("a branch on something not a bool") {
        auto& add{function_named(mod, "add")};
        auto [seg, sum]{first_of(add, gir::instruction_kind::ADD)};
        auto [cseg,
              branch]{first_of(function_named(mod, "pick"), gir::instruction_kind::COND_GOTO)};
        branch.operands.front().type = *sum.type;
        CHECK(reported(mod, "branches on a 'i32', not a 'bool'"));
    }
}

TEST_CASE("The GIR verifier reports locals used without a dominating definition") {
    auto run{helpers::run_uir_sema(PROGRAM)};
    REQUIRE(run->out->succeeded());
    auto& mod{*run->out->gir};
    auto& add{function_named(mod, "add")};
    auto [seg, sum]{first_of(add, gir::instruction_kind::ADD)};

    SECTION("a local nothing defines") {
        auto [rseg, ret]{first_of(add, gir::instruction_kind::RET)};
        ret.operands.front() = gir::value{gir::local_id::make_temp(999), *sum.type};
        CHECK(reported(mod, "uses local %999, which nothing defines"));
    }

    SECTION("a use before its definition") {
        auto& instructions{seg.get_instructions()};
        instructions.insert(instructions.begin(),
                            mod.arena().make<gir::instruction>(gir::instruction{
                                .kind     = gir::instruction_kind::NEG,
                                .type     = *sum.type,
                                .result   = add.next_local_id(),
                                .operands = {gir::value{*sum.result, *sum.type}},
                            }));
        CHECK(reported(mod, "where its definition doesn't dominate it"));
    }

    SECTION("a local defined twice") {
        seg.insert_before_terminator(gir::instruction{
            .kind     = gir::instruction_kind::NEG,
            .type     = *sum.type,
            .result   = *sum.result,
            .operands = {gir::value{*sum.result, *sum.type}},
        });
        CHECK(reported(mod, "redefines local"));
    }

    SECTION("a parameter the function lacks") {
        sum.operands.back() = gir::value{gir::local_id::make_param(42), *sum.type};
        CHECK(reported(mod, "uses parameter %42, which the function lacks"));
    }
}

TEST_CASE("The GIR verifier reports malformed instructions") {
    auto run{helpers::run_uir_sema(PROGRAM)};
    REQUIRE(run->out->succeeded());
    auto& mod{*run->out->gir};

    SECTION("an operand too many") {
        auto [seg, sum]{first_of(function_named(mod, "add"), gir::instruction_kind::ADD)};
        sum.operands.emplace_back(sum.operands.front());
        CHECK(reported(mod, "has 3 operands, expected 2"));
    }

    SECTION("a call with an argument missing") {
        auto [seg, call]{first_of(function_named(mod, "pick"), gir::instruction_kind::CALL)};
        call.operands.pop_back();
        CHECK(reported(mod, "passes 1 arguments to 'add', which takes 2"));
    }

    SECTION("a call to a symbol the module lacks") {
        auto [seg, call]{first_of(function_named(mod, "pick"), gir::instruction_kind::CALL)};
        call.callee_name = std::string{"missing"};
        CHECK(reported(mod, "names 'missing', which the module lacks"));
    }

    SECTION("a return without the value its function returns") {
        auto [seg, ret]{first_of(function_named(mod, "add"), gir::instruction_kind::RET)};
        ret.operands.clear();
        CHECK(reported(mod, "returns nothing from a function returning 'i32'"));
    }
}

} // namespace ghoti::tests
