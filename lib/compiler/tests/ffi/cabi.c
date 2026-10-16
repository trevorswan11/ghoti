/*
 * The C side of the C ABI interop tests in tests/e2e/test_c_abi.cc. Every aggregate gets four entry points:
 * - `check_T`: ghoti passes the canonical value
 * - `make_T`: C returns it
 * - `bump_T`: both directions at once
 * -  `call_T`: C calls a ghoti callback with it
 *
 * Each returns or compares against values that differ per field, so a misplaced or truncated field never matches.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#define FIELDS_EQ1(a, b, f0) ((a).f0 == (b).f0)
#define FIELDS_EQ2(a, b, f0, f1) (FIELDS_EQ1(a, b, f0) && (a).f1 == (b).f1)
#define FIELDS_EQ3(a, b, f0, f1, f2) (FIELDS_EQ2(a, b, f0, f1) && (a).f2 == (b).f2)
#define FIELDS_EQ4(a, b, f0, f1, f2, f3) (FIELDS_EQ3(a, b, f0, f1, f2) && (a).f3 == (b).f3)

#define DEFINE_ABI_SUITE(T, EQ, BUMP, ...)                                     \
    static T canonical_##T(void) { return (T){__VA_ARGS__}; }                  \
    static T bumped_##T(T v) {                                                 \
        BUMP;                                                                  \
        return v;                                                              \
    }                                                                          \
    static bool eq_##T(T a, T b) { return EQ; }                                \
    T           make_##T(void) { return canonical_##T(); }                     \
    int32_t     check_##T(T v) { return eq_##T(v, canonical_##T()); }          \
    T           bump_##T(T v) { return bumped_##T(v); }                        \
    int32_t     call_##T(T (*callback)(T)) {                                   \
        return eq_##T(callback(canonical_##T()), bumped_##T(canonical_##T())); \
    }

typedef struct {
    uint8_t a;
} S1;
typedef struct {
    uint8_t a, b;
} S2;
typedef struct {
    uint8_t a, b, c;
} S3;
typedef struct {
    int16_t a, b;
} S4;
typedef struct {
    int16_t a, b, c;
} S6;
typedef struct {
    int32_t a, b;
} S8;
typedef struct {
    int32_t a, b, c;
} S12;
typedef struct {
    int64_t a, b;
} S16;
typedef struct {
    int64_t a, b, c;
} S24;
typedef struct {
    int64_t a, b, c, d;
} S32;
typedef struct {
    uint8_t bytes[5];
} A5;
typedef struct {
    float x, y;
} F2;
typedef struct {
    float x, y, z;
} F3;
typedef struct {
    float x, y, z, w;
} F4;
typedef struct {
    double x, y;
} D2;
typedef struct {
    double x, y, z;
} D3;
typedef struct {
    double x, y, z, w;
} D4;
typedef struct {
    int32_t i;
    float   f;
} MixIF;
typedef struct {
    float   f;
    int32_t i;
    int32_t j;
} MixFII;
typedef struct {
    double  d;
    int64_t i;
} MixDI;
typedef struct {
    int64_t i;
    double  d;
} MixID;
typedef struct {
    S2       inner;
    uint16_t tail;
} Nested;
typedef union {
    int32_t i;
    float   f;
} UIF;

DEFINE_ABI_SUITE(S1, FIELDS_EQ1(a, b, a), v.a += 1, 11)
DEFINE_ABI_SUITE(S2, FIELDS_EQ2(a, b, a, b), (v.a += 1, v.b += 1), 11, 22)
DEFINE_ABI_SUITE(S3, FIELDS_EQ3(a, b, a, b, c), (v.a += 1, v.b += 1, v.c += 1), 11, 22, 33)
DEFINE_ABI_SUITE(S4, FIELDS_EQ2(a, b, a, b), (v.a += 1, v.b += 1), -1'111, 2'222)
DEFINE_ABI_SUITE(S6, FIELDS_EQ3(a, b, a, b, c), (v.a += 1, v.b += 1, v.c += 1), -11, 22, -33)
DEFINE_ABI_SUITE(S8, FIELDS_EQ2(a, b, a, b), (v.a += 1, v.b += 1), -100'000, 200'000)
DEFINE_ABI_SUITE(S12, FIELDS_EQ3(a, b, a, b, c), (v.a += 1, v.b += 1, v.c += 1), 1, -2, 3)
DEFINE_ABI_SUITE(S16, FIELDS_EQ2(a, b, a, b), (v.a += 1, v.b += 1), -5'000'000'000, 6'000'000'000)
DEFINE_ABI_SUITE(S24,
                 FIELDS_EQ3(a, b, a, b, c),
                 (v.a += 1, v.b += 1, v.c += 1),
                 7'000'000'000,
                 -8'000'000'000,
                 9'000'000'000)
DEFINE_ABI_SUITE(
    S32, FIELDS_EQ4(a, b, a, b, c, d), (v.a += 1, v.b += 1, v.c += 1, v.d += 1), 1, -2, 3, -4)
DEFINE_ABI_SUITE(A5,
                 (a.bytes[0] == b.bytes[0] && a.bytes[1] == b.bytes[1] &&
                  a.bytes[2] == b.bytes[2] && a.bytes[3] == b.bytes[3] && a.bytes[4] == b.bytes[4]),
                 for (int k = 0; k < 5; ++k) { v.bytes[k] += 1; },
                 {1, 2, 3, 4, 5})
DEFINE_ABI_SUITE(F2, FIELDS_EQ2(a, b, x, y), (v.x += 1, v.y += 1), 1.5f, -2.5f)
DEFINE_ABI_SUITE(F3, FIELDS_EQ3(a, b, x, y, z), (v.x += 1, v.y += 1, v.z += 1), 1.5f, -2.5f, 3.5f)
DEFINE_ABI_SUITE(F4,
                 FIELDS_EQ4(a, b, x, y, z, w),
                 (v.x += 1, v.y += 1, v.z += 1, v.w += 1),
                 1.5f,
                 -2.5f,
                 3.5f,
                 -4.5f)
DEFINE_ABI_SUITE(D2, FIELDS_EQ2(a, b, x, y), (v.x += 1, v.y += 1), 1.25, -2.25)
DEFINE_ABI_SUITE(D3, FIELDS_EQ3(a, b, x, y, z), (v.x += 1, v.y += 1, v.z += 1), 1.25, -2.25, 3.25)
DEFINE_ABI_SUITE(D4,
                 FIELDS_EQ4(a, b, x, y, z, w),
                 (v.x += 1, v.y += 1, v.z += 1, v.w += 1),
                 1.25,
                 -2.25,
                 3.25,
                 -4.25)
DEFINE_ABI_SUITE(MixIF, FIELDS_EQ2(a, b, i, f), (v.i += 1, v.f += 1), -7, 8.5f)
DEFINE_ABI_SUITE(MixFII, FIELDS_EQ3(a, b, f, i, j), (v.f += 1, v.i += 1, v.j += 1), 8.5f, -7, 9)
DEFINE_ABI_SUITE(MixDI, FIELDS_EQ2(a, b, d, i), (v.d += 1, v.i += 1), -3.75, 123'456'789'012)
DEFINE_ABI_SUITE(MixID, FIELDS_EQ2(a, b, i, d), (v.i += 1, v.d += 1), 123'456'789'012, -3.75)
DEFINE_ABI_SUITE(Nested,
                 (a.inner.a == b.inner.a && a.inner.b == b.inner.b && a.tail == b.tail),
                 (v.inner.a += 1, v.inner.b += 1, v.tail += 1),
                 {11, 22},
                 33'333)
DEFINE_ABI_SUITE(UIF, FIELDS_EQ1(a, b, i), v.i += 1, .i = 0x12345678)

// Narrow integers must reach C sign or zero extended, whatever the caller left above them
int32_t widen_i8(int8_t v) { return v; }
int32_t widen_u8(uint8_t v) { return v; }
int32_t widen_i16(int16_t v) { return v; }
int32_t widen_u16(uint16_t v) { return v; }
int32_t widen_bool(bool v) { return v ? 1 : 0; }
int8_t  narrow_i8(int32_t v) { return (int8_t)v; }
uint8_t narrow_u8(int32_t v) { return (uint8_t)v; }
bool    is_positive(int32_t v) { return v > 0; }

int64_t sum_i64(int64_t a, int64_t b) { return a + b; }
double  mul_f64(double a, float b) { return a * b; }

// More arguments than any target passes in registers, so the rest spill to the stack
int64_t many_ints(int32_t a,
                  int64_t b,
                  int8_t  c,
                  int16_t d,
                  int32_t e,
                  int64_t f,
                  int32_t g,
                  int64_t h,
                  int32_t i,
                  int64_t j) {
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10;
}

double many_floats(
    double a, float b, double c, float d, double e, float f, double g, float h, double i, float j) {
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10;
}

// An aggregate arriving after the argument registers ran out
int32_t late_struct(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, S16 s) {
    return a + b + c + d + e + f == 21 && s.a == 7 && s.b == 8;
}

int64_t sum_varargs(int32_t count, ...) {
    va_list ap;
    va_start(ap, count);
    int64_t total = 0;
    for (int32_t k = 0; k < count; ++k) { total += va_arg(ap, int64_t); }
    va_end(ap);
    return total;
}

double sum_double_varargs(int32_t count, ...) {
    va_list ap;
    va_start(ap, count);
    double total = 0;
    for (int32_t k = 0; k < count; ++k) { total += va_arg(ap, double); }
    va_end(ap);
    return total;
}

int32_t call_int_callback(int32_t (*callback)(int32_t, int32_t), int32_t a, int32_t b) {
    return callback(a, b) + 1;
}

int32_t global_counter = 41;

int32_t* counter_address(void) { return &global_counter; }

// Larger than ghoti's own by-pointer threshold, so both aggregate passes see it
typedef struct {
    int64_t values[40];
} Big320;

int64_t sum_big(Big320 big) {
    int64_t total = 0;
    for (int k = 0; k < 40; ++k) { total += big.values[k] * (k + 1); }
    return total;
}

Big320 make_big(int64_t seed) {
    Big320 big;
    for (int k = 0; k < 40; ++k) { big.values[k] = seed + k; }
    return big;
}
