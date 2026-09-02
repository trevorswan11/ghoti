R"(type Real = f64
const MAX_SIZE: usize = 100
const Point: struct = struct
const Color: enum : i32 = enum : i32
fn compute_point(p: struct) -> i32
  seg 0:
    %0 = alloca i32
    store 0, %0
    %1 = alloca struct
    store param.0, %1
    %2 = get_element_ptr i32 %1, 0
    %3 = load %2
    %4 = call @clamp__i32_i32_i32(%3, 0, 50)
    %5 = alloca i32
    store 0, %5
    %6 = alloca block
    goto seg 1
  seg 1:
    %7 = load %5
    %8 = lt bool %7, 3
    cond_goto %8 seg 2, seg 3
  seg 2:
    %9 = load %0
    %10 = add i32 %9, %4
    store %10, %0
    %11 = load %5
    %12 = add i32 %11, 1
    store %12, %5
    goto seg 1
  seg 3:
    %13 = load %6
    %14 = load %0
    %15 = get_element_ptr i32 %1, 1
    %16 = load %15
    %17 = add i32 %14, %16
    %18 = load %0
    %19 = add i32 %18, 1
    store %19, %0
    ret i32 %17

fn match_color(c: enum : i32) -> i32
  seg 0:
    %0 = alloca i32
    %1 = eq bool param.0, 0
    cond_goto %1 seg 2, seg 3
  seg 1:
    %4 = load %0
    ret i32 %4
  seg 2:
    store 1, %0
    goto seg 1
  seg 3:
    %2 = eq bool param.0, 1
    cond_goto %2 seg 4, seg 5
  seg 4:
    store 2, %0
    goto seg 1
  seg 5:
    %3 = eq bool param.0, 2
    cond_goto %3 seg 6, seg 7
  seg 6:
    store 3, %0
    goto seg 1
  seg 7:
    unreachable

fn raw_write(fd: i64, buf: ^u8, len: usize) -> i64
  seg 0:
    %0 = alloca i64
    store 0, %0
    inline_asm volatile "syscall" "={rax},{rax},{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}"(1, param.0, param.1, param.2)
    %1 = load %0
    ret i64 %1

test "golden_run"
  seg 0:
    %0 = alloca struct
    %1 = alloca struct
    %2 = get_element_ptr i32 %1, 0
    store 25, %2
    %3 = get_element_ptr i32 %1, 1
    store 10, %3
    %4 = load %1
    store %4, %0
    %5 = load %0
    %6 = call @compute_point(%5)
    ret bool true

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
