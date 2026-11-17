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

} // namespace ghoti::tests
