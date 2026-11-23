#include <catch2/catch_test_macros.hpp>

#include "helpers/gir_effects.hh"

namespace ghoti::tests {

using helpers::same_effects;

TEST_CASE("Reading memory nothing writes later may move among the effects") {
    const auto entry_first{R"(fn f(w: &mut struct, x: usize) -> void
  seg 0:
    %0 = alloca dyn
    store param.0, %0
    %1 = get_element_ptr ^opaque %0, 1
    %2 = load %1
    %3 = add usize param.1, 1
    call %2(%3)
    ret void void
)"};
    const auto args_first{R"(fn f(w: &mut struct, x: usize) -> void
  seg 0:
    %0 = alloca dyn
    %1 = add usize param.1, 1
    store param.0, %0
    %2 = get_element_ptr ^opaque %0, 1
    %3 = load %2
    call %3(%1)
    ret void void
)"};
    CHECK(same_effects(entry_first, args_first));
}

TEST_CASE("A load of a variable written after it starts keeps its place among the effects") {
    const auto before{R"(fn f(x: u32) -> u32
  seg 0:
    %0 = alloca u32
    store 1, %0
    %1 = load %0
    store 2, %0
    ret u32 %1
)"};
    const auto after{R"(fn f(x: u32) -> u32
  seg 0:
    %0 = alloca u32
    store 1, %0
    store 2, %0
    %1 = load %0
    ret u32 %1
)"};
    CHECK_FALSE(same_effects(before, after));
}

TEST_CASE("Effects in a different order are different") {
    const auto ab{R"(fn f() -> void
  seg 0:
    call @a()
    call @b()
    ret void void
)"};
    const auto ba{R"(fn f() -> void
  seg 0:
    call @b()
    call @a()
    ret void void
)"};
    CHECK_FALSE(same_effects(ab, ba));
}

TEST_CASE("A scratch aggregate built from constants reads the constant it was given") {
    const auto built{R"(fn f(v: u16) -> u16
  seg 0:
    %0 = alloca struct
    %1 = alloca struct
    %2 = get_element_ptr u16 %0, 0
    store 4, %2
    %3 = load %0
    store %3, %1
    %4 = get_element_ptr u16 %1, 0
    %5 = load %4
    %6 = sub u16 %5, param.0
    ret u16 %6
)"};
    const auto folded{R"(fn f(v: u16) -> u16
  seg 0:
    %0 = sub u16 4, param.0
    ret u16 %0
)"};
    CHECK(same_effects(built, folded));
}

TEST_CASE("A by-value parameter read through a load is the parameter") {
    const auto loaded{R"(fn f(s: []u8) -> void
  seg 0:
    %0 = load param.0
    call @g(%0)
    ret void void
)"};
    const auto direct{R"(fn f(s: []u8) -> void
  seg 0:
    call @g(param.0)
    ret void void
)"};
    CHECK(same_effects(loaded, direct));
}

TEST_CASE("A slot built field by field and read whole may have its fields stored anywhere") {
    const auto tag_first{R"(fn f(x: u32) -> void
  seg 0:
    %0 = alloca union
    %1 = get_element_ptr i32 %0, 0
    store 0, %1
    %2 = add u32 param.0, 1
    %3 = get_element_ptr u32 %0, 1
    store %2, %3
    %4 = load %0
    call @g(%4)
    ret void void
)"};
    const auto payload_first{R"(fn f(x: u32) -> void
  seg 0:
    %0 = alloca union
    %1 = add u32 param.0, 1
    %2 = get_element_ptr i32 %0, 0
    store 0, %2
    %3 = get_element_ptr u32 %0, 1
    store %1, %3
    %4 = load %0
    call @g(%4)
    ret void void
)"};
    CHECK(same_effects(tag_first, payload_first));
}

TEST_CASE("A stored value that differs is a different effect") {
    const auto one{R"(fn f() -> void
  seg 0:
    %0 = alloca u32
    store 1, %0
    call @g(%0)
    ret void void
)"};
    const auto two{R"(fn f() -> void
  seg 0:
    %0 = alloca u32
    store 2, %0
    call @g(%0)
    ret void void
)"};
    CHECK_FALSE(same_effects(one, two));
}

} // namespace ghoti::tests
