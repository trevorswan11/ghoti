R"(type Real = f64
const MAX_SIZE: usize = 100
const Point: struct = struct
const Color: enum = enum
fn compute_point(p: struct) -> i32
  seg 0:
    %0 = alloca i32
    store 0, %0
    %1 = get_element_ptr i32 param.0, 0
    %2 = load %1
    %3 = call @clamp__i32_i32_i32(%2, 0, 50)
    %4 = alloca i32
    store 0, %4
    %5 = alloca block
    goto seg 1
  seg 1:
    %6 = load %4
    %7 = lt bool %6, 3
    cond_goto %7 seg 2, seg 3
  seg 2:
    %8 = load %0
    %9 = add i32 %8, %3
    store %9, %0
    %10 = load %4
    %11 = add i32 %10, 1
    store %11, %4
    goto seg 1
  seg 3:
    %12 = load %5
    %13 = load %0
    %14 = get_element_ptr i32 param.0, 1
    %15 = load %14
    %16 = add i32 %13, %15
    %17 = load %0
    %18 = add i32 %17, 1
    store %18, %0
    ret i32 %16
    %19 = load %0
    %20 = add i32 %19, 1
    store %20, %0
    ret i32 undefined

fn match_color(c: enum) -> i32
  seg 0:
    %0 = eq bool param.0, 0
    cond_goto %0 seg 2, seg 3
  seg 1:
    ret void void
  seg 2:
    goto seg 1
  seg 3:
    %1 = eq bool param.0, 1
    cond_goto %1 seg 4, seg 5
  seg 4:
    goto seg 1
  seg 5:
    %2 = eq bool param.0, 2
    cond_goto %2 seg 6, seg 7
  seg 6:
    goto seg 1
  seg 7:
    unreachable

test "golden_run"
  seg 0:
    %0 = call @compute_point(void)
    ret void

fn clamp__i32_i32_i32(val: i32, min_val: i32, max_val: i32) -> i32
  seg 0:
    %0 = lt bool param.0, param.1
    cond_goto %0 seg 1, seg 2
  seg 1:
    ret i32 param.1
  seg 2:
    %1 = gt bool param.0, param.2
    cond_goto %1 seg 3, seg 4
  seg 3:
    ret i32 param.2
  seg 4:
    ret i32 param.0

)"
