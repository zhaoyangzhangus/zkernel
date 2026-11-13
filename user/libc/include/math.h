#pragma once

#include <float.h>
#include <limits.h>

#define HUGE_VAL  (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VALL (__builtin_huge_vall())
#ifndef INFINITY
#define INFINITY  (__builtin_inff())
#endif
#ifndef NAN
#define NAN       (__builtin_nanf(""))
#endif

/* ISO C 头文件不要求提供历史 BSD 数学常量，但 Chromium 内置的 DSP 代码以 C11 构建时会使用这些名称。 */
#ifndef M_E
#define M_E 2.71828182845904523536028747135266250
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923132169163975144
#endif
#ifndef M_PI_4
#define M_PI_4 0.78539816339744830961566084581987572
#endif
#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880168872420969808
#endif
#ifndef M_LOG2E
#define M_LOG2E 1.44269504088896340735992468100189214
#endif
#ifndef M_LOG10E
#define M_LOG10E 0.43429448190325182765112891891660508
#endif
#ifndef M_LN2
#define M_LN2 0.69314718055994530941723212145817657
#endif
#ifndef M_LN10
#define M_LN10 2.30258509299404568401799145468436421
#endif
#ifndef M_2PI
#define M_2PI 6.28318530717958647692528676655900576
#endif
#ifndef M_2_SQRTPI
#define M_2_SQRTPI 1.12837916709551257389615890312154517
#endif
#ifndef M_SQRT1_2
#define M_SQRT1_2 0.70710678118654752440084436210484903
#endif

#define MATH_ERRNO     1
#define MATH_ERREXCEPT 2
#define math_errhandling MATH_ERRNO

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
#define FP_ILOGB0    INT_MIN
#define FP_ILOGBNAN  INT_MIN

#if FLT_EVAL_METHOD == 0
typedef float float_t;
typedef double double_t;
#elif FLT_EVAL_METHOD == 1
typedef double float_t;
typedef double double_t;
#else
typedef long double float_t;
typedef long double double_t;
#endif

#define fpclassify(value) \
    __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, \
                         FP_ZERO, (value))
#define isfinite(value) __builtin_isfinite(value)
#define isinf(value) __builtin_isinf_sign(value)
#define isnan(value) __builtin_isnan(value)
#define isnormal(value) __builtin_isnormal(value)
#define signbit(value) __builtin_signbit(value)
#define isgreater(left, right) __builtin_isgreater((left), (right))
#define isgreaterequal(left, right) __builtin_isgreaterequal((left), (right))
#define isless(left, right) __builtin_isless((left), (right))
#define islessequal(left, right) __builtin_islessequal((left), (right))
#define islessgreater(left, right) __builtin_islessgreater((left), (right))
#define isunordered(left, right) __builtin_isunordered((left), (right))

#ifdef __cplusplus
extern "C" {
#endif

#define LIBC_MATH_UNARY(name) \
    float name##f(float value); \
    double name(double value); \
    long double name##l(long double value)
#define LIBC_MATH_BINARY(name) \
    float name##f(float left, float right); \
    double name(double left, double right); \
    long double name##l(long double left, long double right)
#define LIBC_MATH_TERNARY(name) \
    float name##f(float first, float second, float third); \
    double name(double first, double second, double third); \
    long double name##l(long double first, long double second, long double third)

LIBC_MATH_UNARY(fabs);
LIBC_MATH_UNARY(sqrt);
LIBC_MATH_UNARY(cbrt);
LIBC_MATH_UNARY(sin);
LIBC_MATH_UNARY(cos);
void sincosf(float value, float *sine, float *cosine);
void sincos(double value, double *sine, double *cosine);
void sincosl(long double value, long double *sine, long double *cosine);
LIBC_MATH_UNARY(tan);
LIBC_MATH_UNARY(asin);
LIBC_MATH_UNARY(acos);
LIBC_MATH_UNARY(atan);
LIBC_MATH_BINARY(atan2);
LIBC_MATH_UNARY(sinh);
LIBC_MATH_UNARY(cosh);
LIBC_MATH_UNARY(tanh);
LIBC_MATH_UNARY(asinh);
LIBC_MATH_UNARY(acosh);
LIBC_MATH_UNARY(atanh);
LIBC_MATH_UNARY(exp);
LIBC_MATH_UNARY(exp2);
LIBC_MATH_UNARY(expm1);
LIBC_MATH_UNARY(log);
LIBC_MATH_UNARY(log2);
LIBC_MATH_UNARY(log10);
LIBC_MATH_UNARY(log1p);
LIBC_MATH_BINARY(pow);
LIBC_MATH_TERNARY(fma);
LIBC_MATH_UNARY(erf);
LIBC_MATH_UNARY(erfc);
LIBC_MATH_UNARY(lgamma);
LIBC_MATH_UNARY(tgamma);
LIBC_MATH_BINARY(hypot);
LIBC_MATH_UNARY(ceil);
LIBC_MATH_UNARY(floor);
LIBC_MATH_UNARY(trunc);
LIBC_MATH_UNARY(round);
LIBC_MATH_UNARY(rint);
LIBC_MATH_UNARY(nearbyint);
LIBC_MATH_UNARY(logb);
LIBC_MATH_BINARY(fmod);
LIBC_MATH_BINARY(remainder);
LIBC_MATH_BINARY(copysign);
LIBC_MATH_BINARY(fdim);
LIBC_MATH_BINARY(fmax);
LIBC_MATH_BINARY(fmin);

float frexpf(float value, int *exponent);
double frexp(double value, int *exponent);
long double frexpl(long double value, int *exponent);
float modff(float value, float *integer);
double modf(double value, double *integer);
long double modfl(long double value, long double *integer);
int ilogbf(float value);
int ilogb(double value);
int ilogbl(long double value);
float ldexpf(float value, int exponent);
double ldexp(double value, int exponent);
long double ldexpl(long double value, int exponent);
float scalbnf(float value, int exponent);
double scalbn(double value, int exponent);
long double scalbnl(long double value, int exponent);
float scalblnf(float value, long exponent);
double scalbln(double value, long exponent);
long double scalblnl(long double value, long exponent);

long lrintf(float value);
long lrint(double value);
long lrintl(long double value);
long long llrintf(float value);
long long llrint(double value);
long long llrintl(long double value);
long lroundf(float value);
long lround(double value);
long lroundl(long double value);
long long llroundf(float value);
long long llround(double value);
long long llroundl(long double value);

float remquof(float left, float right, int *quotient);
double remquo(double left, double right, int *quotient);
long double remquol(long double left, long double right, int *quotient);
float nextafterf(float value, float toward);
double nextafter(double value, double toward);
long double nextafterl(long double value, long double toward);
float nexttowardf(float value, long double toward);
double nexttoward(double value, long double toward);
long double nexttowardl(long double value, long double toward);
float nanf(const char *tag);
double nan(const char *tag);
long double nanl(const char *tag);
extern int signgam;

#undef LIBC_MATH_UNARY
#undef LIBC_MATH_BINARY
#undef LIBC_MATH_TERNARY

#ifdef __cplusplus
}
#endif
