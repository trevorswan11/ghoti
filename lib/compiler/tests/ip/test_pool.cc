#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/arena.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/sema.hh"
#include "support/big_int.hh"
#include "support/float128.hh"
#include "support/int128.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] auto int_key(u16 bits, bool is_signed) -> sema::types::key_t {
    return {sema::type_kind::INT, sema::types::mut::CONSTANT, bits, is_signed};
}

// A value interned, then read back as the evaluator represents it
[[nodiscard]] auto round_trip(ip::pool& pool, const gir::const_value& value) -> gir::const_value {
    return gir::const_value::from_interned(pool, UNWRAP(value.intern(pool)));
}

} // namespace

TEST_CASE("A type is created once per key and found again by its index") {
    ghoti::arena    arena;
    sema::type_pool types{arena};
    auto&           pool{types.intern()};

    auto& i32_type{*types[int_key(32, true)]};
    CHECK(&*types[int_key(32, true)] == &i32_type);
    CHECK(&*types[int_key(32, false)] != &i32_type);
    CHECK(pool.is_type(i32_type.get_ip_index()));
    CHECK(&pool.type_at(i32_type.get_ip_index()) == &i32_type);
}

TEST_CASE("Equal values share an index, and a different type or payload doesn't") {
    ghoti::arena    arena;
    sema::type_pool types{arena};
    auto&           pool{types.intern()};
    auto&           i32_type{*types[int_key(32, true)]};
    auto&           i64_type{*types[int_key(64, true)]};

    const auto five{pool.intern(i64{5}, i32_type)};
    CHECK(pool.intern(i64{5}, i32_type) == five);
    CHECK(pool.intern(i64{6}, i32_type) != five);
    CHECK(pool.intern(i64{5}, i64_type) != five);
    CHECK(pool.intern(i64{5}) != five);
    // The width an integer was written in is part of it, so it reads back in that width
    CHECK(pool.intern(u64{5}, i32_type) != five);
    CHECK_FALSE(pool.is_type(five));
    CHECK(pool.value_at(five).data.as<i64>() == 5);
    CHECK(&*pool.value_at(five).type == &i32_type);
}

TEST_CASE("Floats are interned by bit pattern") {
    ghoti::arena    arena;
    sema::type_pool types{arena};
    auto&           pool{types.intern()};

    CHECK(pool.intern(f128::zero()) != pool.intern(f128::zero(true)));
    CHECK(pool.intern(f128::from_f64(1.5)) == pool.intern(f128::from_f64(1.5)));
    const auto nan{f128::quiet_nan()};
    CHECK(pool.intern(nan) == pool.intern(nan));
}

TEST_CASE("Strings, markers, and composite values each get their own entry") {
    ghoti::arena    arena;
    sema::type_pool types{arena};
    auto&           pool{types.intern()};
    auto&           i32_type{*types[int_key(32, true)]};

    CHECK(pool.intern(std::string{"ab"}) == pool.intern(std::string{"ab"}));
    CHECK(pool.intern(std::string{"ab"}) != pool.intern(std::string{"a"}));
    CHECK(pool.intern(ip::void_value{}) != pool.intern(ip::undefined_value{}));
    CHECK(pool.intern(ip::zero_value{}, i32_type) != pool.intern(ip::zero_value{}));

    const auto one{pool.intern(i64{1}, i32_type)};
    const auto two{pool.intern(i64{2}, i32_type)};
    const auto pair{pool.intern(ip::aggregate_value{{one, two}})};
    CHECK(pool.intern(ip::aggregate_value{{one, two}}) == pair);
    CHECK(pool.intern(ip::aggregate_value{{two, one}}) != pair);
    CHECK(pool.intern(ip::aggregate_value{{one, stdx::none}}) != pair);
    CHECK(pool.intern(ip::union_value{"a", {one}}) != pool.intern(ip::union_value{"b", {one}}));
    CHECK(pool.intern(ip::address_value{"g", {}}) != pool.intern(ip::address_value{"g", {one}}));
}

TEST_CASE("Evaluator values come back from the pool as they went in") {
    ghoti::arena    arena;
    sema::type_pool types{arena};
    auto&           pool{types.intern()};
    auto&           i32_type{*types[int_key(32, true)]};
    using gir::const_value;

    const auto& value{
        GENERATE_REF(const_value{i64{-3}, i32_type},
                     const_value{u64{7}},
                     const_value{i128{-1} << 100},
                     const_value{u128{1} << 120},
                     const_value{big_int::parse("1" + std::string(50, '0'), 10).value()},
                     const_value{true},
                     const_value{std::string{"text"}},
                     const_value{gir::const_enum{"red", 2}, i32_type},
                     const_value{gir::const_union{"some", {const_value{i64{4}}}}},
                     const_value{gir::const_addr{"global", {}}},
                     const_value{gir::const_dyn_fat_ptr{"data", "vtable"}},
                     const_value{gir::const_array{{const_value{i64{1}}, const_value{i64{2}}}}},
                     const_value{gir::void_val{}},
                     const_value{gir::undefined_val{}},
                     const_value{gir::nullptr_val{}},
                     const_value{gir::poison_val{}},
                     const_value{i32_type})};

    const auto back{round_trip(pool, value)};
    CHECK(back.identical(value));
    CHECK(back.get_type().transform([](const sema::type& t) { return &t; }) ==
          value.get_type().transform([](const sema::type& t) { return &t; }));
}

TEST_CASE("A type as a value is the type's own entry") {
    ghoti::arena    arena;
    sema::type_pool types{arena};
    auto&           pool{types.intern()};
    auto&           i32_type{*types[int_key(32, true)]};

    CHECK(UNWRAP(gir::const_value{i32_type}.intern(pool)) == i32_type.get_ip_index());
}

TEST_CASE("A struct value is held in field order, with unset fields left out") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Point = struct {
            x: i32,
            y: i32,
            z: bool,
        };
    )")};
    auto [_1, _2, point_type]{ctx->get_type_sym_info<sema::symbols::node_t>("Point", idx)};
    auto& point{sema::denoted_type(point_type)};
    auto& pool{ctx->analyzer.get_pool().intern()};

    gir::const_struct fields;
    fields.fields.emplace("z", gir::const_value{true});
    fields.fields.emplace("x", gir::const_value{i64{1}});
    const gir::const_value value{std::move(fields), point};

    const auto  at{UNWRAP(value.intern(pool))};
    const auto& stored{pool.value_at(at).data.as<ip::aggregate_value>()};
    REQUIRE(stored.elements.size() == 3);
    CHECK(pool.value_at(*stored.elements[0]).data.as<i64>() == 1);
    CHECK_FALSE(stored.elements[1]);
    CHECK(pool.value_at(*stored.elements[2]).data.as<bool>());
    CHECK(gir::const_value::from_interned(pool, at) == value);
}

TEST_CASE("What the pool doesn't hold is refused rather than guessed at") {
    ghoti::arena    arena;
    sema::type_pool types{arena};
    auto&           pool{types.intern()};

    gir::const_struct untyped;
    untyped.fields.emplace("x", gir::const_value{i64{1}});
    CHECK_FALSE(gir::const_value{std::move(untyped)}.intern(pool));
    CHECK_FALSE(gir::const_value{gir::const_ref{}}.intern(pool));
}

} // namespace ghoti::tests
