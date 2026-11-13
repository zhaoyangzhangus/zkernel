#pragma once

#include <math.h>
#include <complex.h>

/* GCC 泛型选择器会遵循 C11 规则：复数操作数选择复数版本，实数操作数则保留其精度。 */
#define LIBC_TGMATH_COMPLEX(value, real_name, complex_name) \
    __builtin_tgmath(real_name##f, real_name, real_name##l, \
                     complex_name##f, complex_name, complex_name##l, (value))
#define LIBC_TGMATH_COMPLEX2(left, right, real_name, complex_name) \
    __builtin_tgmath(real_name##f, real_name, real_name##l, \
                     complex_name##f, complex_name, complex_name##l, \
                     (left), (right))
#define LIBC_TGMATH_REAL(value, name) \
    __builtin_tgmath(name##f, name, name##l, (value))
#define LIBC_TGMATH_REAL2(left, right, name) \
    __builtin_tgmath(name##f, name, name##l, (left), (right))
#define LIBC_TGMATH_REAL3(first, second, third, name) \
    __builtin_tgmath(name##f, name, name##l, (first), (second), (third))
#define LIBC_TGMATH_COMPLEX_ONLY(value, name) \
    __builtin_tgmath(name##f, name, name##l, (value))

#define acos(value) LIBC_TGMATH_COMPLEX(value, acos, cacos)
#define asin(value) LIBC_TGMATH_COMPLEX(value, asin, casin)
#define atan(value) LIBC_TGMATH_COMPLEX(value, atan, catan)
#define acosh(value) LIBC_TGMATH_COMPLEX(value, acosh, cacosh)
#define asinh(value) LIBC_TGMATH_COMPLEX(value, asinh, casinh)
#define atanh(value) LIBC_TGMATH_COMPLEX(value, atanh, catanh)
#define cos(value) LIBC_TGMATH_COMPLEX(value, cos, ccos)
#define sin(value) LIBC_TGMATH_COMPLEX(value, sin, csin)
#define tan(value) LIBC_TGMATH_COMPLEX(value, tan, ctan)
#define cosh(value) LIBC_TGMATH_COMPLEX(value, cosh, ccosh)
#define sinh(value) LIBC_TGMATH_COMPLEX(value, sinh, csinh)
#define tanh(value) LIBC_TGMATH_COMPLEX(value, tanh, ctanh)
#define exp(value) LIBC_TGMATH_COMPLEX(value, exp, cexp)
#define log(value) LIBC_TGMATH_COMPLEX(value, log, clog)
#define sqrt(value) LIBC_TGMATH_COMPLEX(value, sqrt, csqrt)
#define fabs(value) LIBC_TGMATH_COMPLEX(value, fabs, cabs)
#define pow(left, right) \
    LIBC_TGMATH_COMPLEX2(left, right, pow, cpow)

#define atan2(left, right) LIBC_TGMATH_REAL2(left, right, atan2)
#define cbrt(value) LIBC_TGMATH_REAL(value, cbrt)
#define ceil(value) LIBC_TGMATH_REAL(value, ceil)
#define copysign(left, right) LIBC_TGMATH_REAL2(left, right, copysign)
#define exp2(value) LIBC_TGMATH_REAL(value, exp2)
#define expm1(value) LIBC_TGMATH_REAL(value, expm1)
#define erf(value) LIBC_TGMATH_REAL(value, erf)
#define erfc(value) LIBC_TGMATH_REAL(value, erfc)
#define fdim(left, right) LIBC_TGMATH_REAL2(left, right, fdim)
#define fma(first, second, third) \
    LIBC_TGMATH_REAL3(first, second, third, fma)
#define floor(value) LIBC_TGMATH_REAL(value, floor)
#define fmax(left, right) LIBC_TGMATH_REAL2(left, right, fmax)
#define fmin(left, right) LIBC_TGMATH_REAL2(left, right, fmin)
#define fmod(left, right) LIBC_TGMATH_REAL2(left, right, fmod)
#define frexp(value, exponent) LIBC_TGMATH_REAL2(value, exponent, frexp)
#define hypot(left, right) LIBC_TGMATH_REAL2(left, right, hypot)
#define ilogb(value) LIBC_TGMATH_REAL(value, ilogb)
#define ldexp(value, exponent) LIBC_TGMATH_REAL2(value, exponent, ldexp)
#define log10(value) LIBC_TGMATH_REAL(value, log10)
#define log1p(value) LIBC_TGMATH_REAL(value, log1p)
#define log2(value) LIBC_TGMATH_REAL(value, log2)
#define logb(value) LIBC_TGMATH_REAL(value, logb)
#define lgamma(value) LIBC_TGMATH_REAL(value, lgamma)
#define lrint(value) LIBC_TGMATH_REAL(value, lrint)
#define lround(value) LIBC_TGMATH_REAL(value, lround)
#define nearbyint(value) LIBC_TGMATH_REAL(value, nearbyint)
#define nextafter(left, right) LIBC_TGMATH_REAL2(left, right, nextafter)
#define nexttoward(left, right) LIBC_TGMATH_REAL2(left, right, nexttoward)
#define remainder(left, right) LIBC_TGMATH_REAL2(left, right, remainder)
#define remquo(left, right, quotient) \
    LIBC_TGMATH_REAL3(left, right, quotient, remquo)
#define rint(value) LIBC_TGMATH_REAL(value, rint)
#define round(value) LIBC_TGMATH_REAL(value, round)
#define scalbn(value, exponent) LIBC_TGMATH_REAL2(value, exponent, scalbn)
#define scalbln(value, exponent) LIBC_TGMATH_REAL2(value, exponent, scalbln)
#define trunc(value) LIBC_TGMATH_REAL(value, trunc)

#define carg(value) LIBC_TGMATH_COMPLEX_ONLY(value, carg)
#define cimag(value) LIBC_TGMATH_COMPLEX_ONLY(value, cimag)
#define conj(value) LIBC_TGMATH_COMPLEX_ONLY(value, conj)
#define cproj(value) LIBC_TGMATH_COMPLEX_ONLY(value, cproj)
#define creal(value) LIBC_TGMATH_COMPLEX_ONLY(value, creal)
