#include <catch2/catch_test_macros.hpp>

#include "helpers/gir_normalize.hh"

namespace ghoti::tests {

using helpers::normalize_gir;

TEST_CASE("Normalized GIR numbers segments in CFG order and threads plain jumps") {
    const auto created_out_of_order{normalize_gir(R"(fn f(c: bool) -> i32
  seg 0:
    cond_goto param.0 seg 2, seg 1
  seg 1:
    goto seg 3
  seg 2:
    ret i32 1
  seg 3:
    ret i32 2
)")};
    const auto in_order{normalize_gir(R"(fn f(c: bool) -> i32
  seg 0:
    cond_goto param.0 seg 1, seg 2
  seg 1:
    ret i32 1
  seg 2:
    ret i32 2
)")};
    CHECK(created_out_of_order == in_order);
}

TEST_CASE("Normalized GIR folds constant integer instructions and branches") {
    const auto folded{normalize_gir(R"(fn f() -> i32
  seg 0:
    %0 = add i32 2, 3
    %1 = gt bool %0, 4
    cond_goto %1 seg 1, seg 2
  seg 1:
    ret i32 %0
  seg 2:
    ret i32 0
)")};
    CHECK(folded == "fn f() -> i32\n  seg 0:\n    ret i32 5\n\n");
}

TEST_CASE("Normalized GIR keeps a checked operation that would overflow") {
    const auto kept{normalize_gir(R"(fn f() -> i8
  seg 0:
    %0 = add.checked i8 127, 1
    ret i8 %0
)")};
    CHECK(kept == "fn f() -> i8\n  seg 0:\n    %0 = add.checked i8 127, 1\n    ret i8 %0\n\n");
}

TEST_CASE("Normalized GIR reads a slot that only ever holds one constant as that constant") {
    const auto forwarded{normalize_gir(R"(fn f() -> i32
  seg 0:
    %0 = alloca i32
    store 7, %0
    goto seg 1
  seg 1:
    %1 = load %0
    ret i32 %1
)")};
    CHECK(forwarded == "fn f() -> i32\n  seg 0:\n    ret i32 7\n\n");
}

TEST_CASE("Normalized GIR hoists allocas and renumbers temporaries") {
    const auto hoisted{normalize_gir(R"(fn f(c: bool) -> i32
  seg 0:
    %4 = load %9
    %9 = alloca i32
    ret i32 %4
)")};
    CHECK(hoisted == "fn f(c: bool) -> i32\n  seg 0:\n    %0 = alloca i32\n    %1 = load %0\n"
                     "    ret i32 %1\n\n");
}

TEST_CASE("Normalized GIR drops a result slot nothing reads") {
    const auto with_slot{normalize_gir(R"(fn f(x: i32) -> i32
  seg 0:
    %0 = alloca i32
    %1 = add i32 param.0, 1
    store %1, %0
    %2 = load %0
    ret i32 %1
)")};
    const auto without{normalize_gir(R"(fn f(x: i32) -> i32
  seg 0:
    %0 = add i32 param.0, 1
    ret i32 %0
)")};
    CHECK(with_slot == without);
}

TEST_CASE("Normalized GIR numbers slots by first use, not by creation") {
    const auto created_first{normalize_gir(R"(fn f(x: i32) -> i32
  seg 0:
    %0 = alloca i32
    %1 = alloca i32
    store param.0, %1
    store 2, %0
    %2 = load %1
    %3 = load %0
    %4 = add i32 %2, %3
    ret i32 %4
)")};
    const auto created_second{normalize_gir(R"(fn f(x: i32) -> i32
  seg 0:
    %0 = alloca i32
    store param.0, %0
    %1 = alloca i32
    store 2, %1
    %2 = load %0
    %3 = load %1
    %4 = add i32 %2, %3
    ret i32 %4
)")};
    CHECK(created_first == created_second);
}

TEST_CASE("Normalized GIR keeps a store through a field's address") {
    const auto kept{normalize_gir(R"(fn f(x: i32) -> struct
  seg 0:
    %0 = alloca struct
    %1 = get_element_ptr i32 %0, 0
    store param.0, %1
    %2 = load %0
    ret struct %2
)")};
    CHECK(kept.contains("store param.0, %1"));
}

TEST_CASE("Normalized GIR computes an address just before its first use") {
    const auto early{normalize_gir(R"(fn f(x: i32) -> struct
  seg 0:
    %0 = alloca struct
    %1 = get_element_ptr i32 %0, 0
    %2 = call @g(param.0)
    store %2, %1
    %3 = load %0
    ret struct %3
)")};
    const auto late{normalize_gir(R"(fn f(x: i32) -> struct
  seg 0:
    %0 = alloca struct
    %1 = call @g(param.0)
    %2 = get_element_ptr i32 %0, 0
    store %1, %2
    %3 = load %0
    ret struct %3
)")};
    CHECK(early == late);
}

TEST_CASE("Normalized GIR orders addresses feeding one instruction by its operands") {
    const auto pointer_first{normalize_gir(R"(fn f() -> i32
  seg 0:
    %0 = alloca i32
    %1 = alloca struct
    %2 = address_of &mut i32 %0
    %3 = get_element_ptr &mut i32 %1, 0
    store %2, %3
    ret i32 0
)")};
    const auto field_first{normalize_gir(R"(fn f() -> i32
  seg 0:
    %0 = alloca i32
    %1 = alloca struct
    %2 = get_element_ptr &mut i32 %1, 0
    %3 = address_of &mut i32 %0
    store %3, %2
    ret i32 0
)")};
    CHECK(pointer_first == field_first);
}

TEST_CASE("Normalized GIR reads a value at its use unless a write comes between") {
    const auto moved{normalize_gir(R"(fn f(s: []u8, i: usize) -> bool
  seg 0:
    %0 = load param.1
    %1 = get_element_ptr usize param.0, 1
    %2 = load %1
    %3 = lt bool %0, %2
    ret bool %3
)")};
    const auto in_place{normalize_gir(R"(fn f(s: []u8, i: usize) -> bool
  seg 0:
    %0 = get_element_ptr usize param.0, 1
    %1 = load %0
    %2 = load param.1
    %3 = lt bool %2, %1
    ret bool %3
)")};
    CHECK(moved == in_place);

    const auto written{normalize_gir(R"(fn f(p: ^i32) -> i32
  seg 0:
    %0 = load param.0
    store 5, param.0
    %1 = add i32 %0, 1
    ret i32 %1
)")};
    CHECK(written.find("load param.0") < written.find("store 5"));
}

TEST_CASE("Normalized GIR delays a constant store to a private slot until the slot is used") {
    const auto early{normalize_gir(R"(fn f(s: []u8) -> bool
  seg 0:
    %0 = alloca bool
    store false, %0
    %1 = get_element_ptr usize param.0, 1
    %2 = load %1
    %3 = lt bool 0, %2
    cond_goto %3 seg 1, seg 2
  seg 1:
    store true, %0
    goto seg 2
  seg 2:
    %4 = load %0
    ret bool %4
)")};
    const auto late{normalize_gir(R"(fn f(s: []u8) -> bool
  seg 0:
    %0 = alloca bool
    %1 = get_element_ptr usize param.0, 1
    %2 = load %1
    %3 = lt bool 0, %2
    store false, %0
    cond_goto %3 seg 1, seg 2
  seg 1:
    store true, %0
    goto seg 2
  seg 2:
    %4 = load %0
    ret bool %4
)")};
    CHECK(early == late);
}

TEST_CASE("Normalized GIR folds an untyped operation left for runtime") {
    const auto folded{normalize_gir(R"(fn f() -> i32
  seg 0:
    %0 = add comptime_int 10, 1
    ret i32 %0
)")};
    CHECK(folded == "fn f() -> i32\n  seg 0:\n    ret i32 11\n\n");
}

TEST_CASE("Normalized GIR fills a private temporary's fields at its next use") {
    // The old emitter stores each field as it computes it, UIR computes them all first
    const auto interleaved{normalize_gir(R"(fn f(a: i32, b: i32) -> struct
  seg 0:
    %0 = alloca struct
    %1 = add.checked i32 param.0, 1
    %2 = get_element_ptr i32 %0, 0
    store %1, %2
    %3 = add.checked i32 param.1, 2
    %4 = get_element_ptr i32 %0, 1
    store %3, %4
    %5 = load %0
    ret struct %5
)")};
    const auto grouped{normalize_gir(R"(fn f(a: i32, b: i32) -> struct
  seg 0:
    %0 = alloca struct
    %1 = add.checked i32 param.0, 1
    %2 = add.checked i32 param.1, 2
    %3 = get_element_ptr i32 %0, 0
    store %1, %3
    %4 = get_element_ptr i32 %0, 1
    store %2, %4
    %5 = load %0
    ret struct %5
)")};
    CHECK(interleaved == grouped);
}

TEST_CASE("Normalized GIR keeps a field store after a read of that field") {
    const auto before{normalize_gir(R"(fn f() -> i32
  seg 0:
    %0 = alloca struct
    %1 = get_element_ptr i32 %0, 0
    store 1, %1
    %2 = load %1
    %3 = get_element_ptr i32 %0, 0
    store 2, %3
    %4 = load %3
    %5 = add.checked i32 %2, %4
    ret i32 %5
)")};
    CHECK(before.find("store 1") < before.find("load"));
    CHECK(before.find("store 2") > before.find("load"));
}

} // namespace ghoti::tests
