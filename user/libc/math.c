#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>

#define PI       3.141592653589793238462643383279502884L
#define TWO_PI   6.283185307179586476925286766559005768L
#define LN2      0.693147180559945309417232121458176568L
#define LOG10_2  0.301029995663981195213738894724493027L

#define X87_ROUND_DOWN    0x400U
#define X87_ROUND_UP      0x800U
#define X87_ROUND_ZERO    0xC00U

static long double x87_sqrt(long double value) {
    __asm__ volatile ("fsqrt" : "+t"(value));
    return value;
}

static long double x87_sin(long double value) {
    __asm__ volatile ("fsin" : "+t"(value));
    return value;
}

static long double x87_cos(long double value) {
    __asm__ volatile ("fcos" : "+t"(value));
    return value;
}

static long double x87_atan2(long double y, long double x) {
    long double result;
    __asm__ volatile ("fpatan" : "=t"(result) : "0"(x), "u"(y) : "st(1)");
    return result;
}

static long double x87_log2(long double value) {
    long double result;
    __asm__ volatile ("fyl2x" : "=t"(result) : "0"(value), "u"(1.0L) : "st(1)");
    return result;
}

static long double x87_scale(long double value, long double exponent) {
    __asm__ volatile ("fscale" : "+t"(value) : "u"(exponent));
    return value;
}

static long double x87_round(long double value, unsigned short mode) {
    unsigned short old_control;
    unsigned short new_control;
    __asm__ volatile ("fnstcw %0" : "=m"(old_control));
    new_control = (unsigned short)((old_control & ~0xC00U) | mode);
    __asm__ volatile ("fldcw %2; frndint; fldcw %1"
                      : "+t"(value) : "m"(old_control), "m"(new_control));
    return value;
}

static long double x87_round_current(long double value) {
    __asm__ volatile ("frndint" : "+t"(value));
    return value;
}

static long double x87_remainder(long double left, long double right,
                                 bool nearest) {
    unsigned short status;
    do {
        if (nearest) {
            __asm__ volatile ("fprem1; fnstsw %%ax"
                              : "+t"(left), "=a"(status) : "u"(right) : "cc");
        } else {
            __asm__ volatile ("fprem; fnstsw %%ax"
                              : "+t"(left), "=a"(status) : "u"(right) : "cc");
        }
    } while ((status & 0x0400U) != 0U);
    return left;
}

static int remquo_low_bits(long double left, long double right) {
    long double modulus = fabsl(right) * 8.0L;
    long double reduced = isinf(modulus) ? left : fmodl(left, modulus);
    long double rounded = x87_round(reduced / right, 0U);
    return (int)rounded % 8;
}

static long double math_domain(void) {
    errno = EDOM;
    return __builtin_nanl("");
}

long double fabsl(long double value) {
    return signbit(value) ? -value : value;
}

long double sqrtl(long double value) {
    if (value < 0.0L) return math_domain();
    return x87_sqrt(value);
}

long double fmodl(long double left, long double right) {
    if (right == 0.0L || isinf(left)) return math_domain();
    if (isnan(left) || isnan(right)) return left + right;
    if (isinf(right)) return left;
    return x87_remainder(left, right, false);
}

long double remainderl(long double left, long double right) {
    if (right == 0.0L || isinf(left)) return math_domain();
    if (isnan(left) || isnan(right)) return left + right;
    if (isinf(right)) return left;
    return x87_remainder(left, right, true);
}

long double remquol(long double left, long double right, int *quotient) {
    if (quotient == 0) {
        errno = EINVAL;
        return __builtin_nanl("");
    }
    *quotient = 0;
    if (right == 0.0L || isinf(left)) return math_domain();
    if (isnan(left) || isnan(right)) return left + right;
    if (isinf(right)) return left;
    *quotient = remquo_low_bits(left, right);
    return x87_remainder(left, right, true);
}

static long double reduce_angle(long double value) {
    return fabsl(value) > PI ? fmodl(value, TWO_PI) : value;
}

long double sinl(long double value) {
    if (isinf(value)) return math_domain();
    return isnan(value) ? value : x87_sin(reduce_angle(value));
}

long double cosl(long double value) {
    if (isinf(value)) return math_domain();
    return isnan(value) ? value : x87_cos(reduce_angle(value));
}

long double tanl(long double value) {
    if (isinf(value)) return math_domain();
    value = reduce_angle(value);
    return x87_sin(value) / x87_cos(value);
}

long double atan2l(long double y, long double x) {
    if (isnan(x) || isnan(y)) return x + y;
    return x87_atan2(y, x);
}

long double atanl(long double value) {
    return atan2l(value, 1.0L);
}

long double asinl(long double value) {
    if (fabsl(value) > 1.0L) return math_domain();
    return atan2l(value, sqrtl((1.0L - value) * (1.0L + value)));
}

long double acosl(long double value) {
    if (fabsl(value) > 1.0L) return math_domain();
    return atan2l(sqrtl((1.0L - value) * (1.0L + value)), value);
}

long double floorl(long double value) {
    return x87_round(value, X87_ROUND_DOWN);
}

long double ceill(long double value) {
    return x87_round(value, X87_ROUND_UP);
}

long double truncl(long double value) {
    return x87_round(value, X87_ROUND_ZERO);
}

long double roundl(long double value) {
    if (!isfinite(value) || value == 0.0L) return value;
    return value < 0.0L ? ceill(value - 0.5L) : floorl(value + 0.5L);
}

long double rintl(long double value) {
    return x87_round_current(value);
}

long double nearbyintl(long double value) {
    /* System 当前仅声明通过 errno 处理数学错误，因此 rint() 的 FE_INEXACT 区别对外不可见。 */
    return x87_round_current(value);
}

long double modfl(long double value, long double *integer) {
    if (isnan(value)) {
        *integer = value;
        return value;
    }
    if (isinf(value)) {
        *integer = value;
        return copysignl(0.0L, value);
    }
    *integer = truncl(value);
    long double fraction = value - *integer;
    return fraction == 0.0L ? copysignl(0.0L, value) : fraction;
}

static long checked_long_integer(long double value) {
    if (!isfinite(value)) {
        errno = EDOM;
        return LONG_MIN;
    }
    if (value < (long double)LONG_MIN || value > (long double)LONG_MAX) {
        errno = ERANGE;
        return value < 0.0L ? LONG_MIN : LONG_MAX;
    }
    return (long)value;
}

static long long checked_long_long_integer(long double value) {
    if (!isfinite(value)) {
        errno = EDOM;
        return LLONG_MIN;
    }
    if (value < (long double)LLONG_MIN || value > (long double)LLONG_MAX) {
        errno = ERANGE;
        return value < 0.0L ? LLONG_MIN : LLONG_MAX;
    }
    return (long long)value;
}

long lrintl(long double value) {
    return checked_long_integer(rintl(value));
}

long long llrintl(long double value) {
    return checked_long_long_integer(rintl(value));
}

long lroundl(long double value) {
    return checked_long_integer(roundl(value));
}

long long llroundl(long double value) {
    return checked_long_long_integer(roundl(value));
}

long double exp2l(long double value) {
    if (isnan(value) || isinf(value)) return value < 0.0L ? 0.0L : value;
    if (value >= (long double)LDBL_MAX_EXP) {
        errno = ERANGE;
        return HUGE_VALL;
    }
    if (value < (long double)(LDBL_MIN_EXP - LDBL_MANT_DIG)) {
        errno = ERANGE;
        return 0.0L;
    }
    long double integer = floorl(value);
    long double fraction = value - integer;
    long double result;
    __asm__ volatile ("f2xm1" : "=t"(result) : "0"(fraction));
    return x87_scale(result + 1.0L, integer);
}

long double expl(long double value) {
    return exp2l(value / LN2);
}

long double expm1l(long double value) {
    if (fabsl(value) >= 0.0001L) return expl(value) - 1.0L;
    long double term = value;
    long double result = value;
    for (unsigned int divisor = 2U; divisor <= 12U; ++divisor) {
        term *= value / (long double)divisor;
        result += term;
    }
    return result;
}

long double log2l(long double value) {
    if (value < 0.0L) return math_domain();
    if (value == 0.0L) {
        errno = ERANGE;
        return -HUGE_VALL;
    }
    return x87_log2(value);
}

long double logl(long double value) {
    return log2l(value) * LN2;
}

long double log10l(long double value) {
    return log2l(value) * LOG10_2;
}

long double log1pl(long double value) {
    if (value < -1.0L) return math_domain();
    if (value == -1.0L) {
        errno = ERANGE;
        return -HUGE_VALL;
    }
    if (fabsl(value) >= 0.0001L) return logl(1.0L + value);
    long double power = value;
    long double result = value;
    for (unsigned int divisor = 2U; divisor <= 20U; ++divisor) {
        power *= value;
        long double term = power / (long double)divisor;
        result += (divisor & 1U) != 0U ? term : -term;
    }
    return result;
}

long double frexpl(long double value, int *exponent) {
    if (exponent == 0) {
        errno = EINVAL;
        return value;
    }
    if (value == 0.0L || !isfinite(value)) {
        *exponent = 0;
        return value;
    }
    long double raw_exponent;
    long double significand;
    __asm__ volatile ("fxtract" : "=t"(significand), "=u"(raw_exponent)
                      : "0"(value));
    *exponent = (int)raw_exponent + 1;
    return significand * 0.5L;
}

int ilogbl(long double value) {
    if (value == 0.0L) return FP_ILOGB0;
    if (isnan(value)) return FP_ILOGBNAN;
    if (isinf(value)) return INT_MAX;
    int exponent;
    (void)frexpl(value, &exponent);
    return exponent - 1;
}

long double logbl(long double value) {
    if (value == 0.0L) {
        errno = ERANGE;
        return -HUGE_VALL;
    }
    if (isnan(value) || isinf(value)) return fabsl(value);
    return (long double)ilogbl(value);
}

long double scalblnl(long double value, long exponent) {
    if (value == 0.0L || !isfinite(value)) return value;
    int value_exponent = ilogbl(value);
    long overflow_limit = (long)(LDBL_MAX_EXP - 1 - value_exponent);
    long underflow_limit =
        (long)(LDBL_MIN_EXP - LDBL_MANT_DIG - value_exponent);
    if (exponent > overflow_limit) {
        errno = ERANGE;
        return copysignl(HUGE_VALL, value);
    }
    if (exponent < underflow_limit) {
        errno = ERANGE;
        return copysignl(0.0L, value);
    }
    long double result = x87_scale(value, (long double)exponent);
    if (isinf(result) || result == 0.0L ||
        fpclassify(result) == FP_SUBNORMAL) errno = ERANGE;
    return result;
}

long double scalbnl(long double value, int exponent) {
    return scalblnl(value, (long)exponent);
}

long double ldexpl(long double value, int exponent) {
    return scalbnl(value, exponent);
}

long double cbrtl(long double value) {
    if (value == 0.0L || !isfinite(value)) return value;
    long double result = exp2l(log2l(fabsl(value)) / 3.0L);
    return value < 0.0L ? -result : result;
}

long double hypotl(long double left, long double right) {
    left = fabsl(left);
    right = fabsl(right);
    if (isinf(left) || isinf(right)) return HUGE_VALL;
    if (isnan(left) || isnan(right)) return left + right;
    if (left < right) {
        long double temporary = left;
        left = right;
        right = temporary;
    }
    if (left == 0.0L) return 0.0L;
    long double ratio = right / left;
    return left * sqrtl(1.0L + ratio * ratio);
}

long double powl(long double base, long double exponent) {
    if (exponent == 0.0L || base == 1.0L) return 1.0L;
    if (isnan(base) || isnan(exponent)) return base + exponent;
    if (isinf(exponent)) {
        long double magnitude = fabsl(base);
        if (magnitude == 1.0L) return 1.0L;
        return (magnitude > 1.0L) == (exponent > 0.0L) ? HUGE_VALL : 0.0L;
    }
    if (base == 0.0L) {
        if (exponent < 0.0L) {
            errno = ERANGE;
            return signbit(base) && fmodl(exponent, 2.0L) != 0.0L ?
                   -HUGE_VALL : HUGE_VALL;
        }
        return signbit(base) && fmodl(exponent, 2.0L) != 0.0L ? -0.0L : 0.0L;
    }
    bool negative = base < 0.0L;
    if (negative && exponent != truncl(exponent)) return math_domain();
    long double result = exp2l(exponent * log2l(fabsl(base)));
    return negative && fmodl(exponent, 2.0L) != 0.0L ? -result : result;
}

long double sinhl(long double value) {
    if (fabsl(value) < 0.0001L) return value + value * value * value / 6.0L;
    long double positive = expl(value);
    return (positive - 1.0L / positive) * 0.5L;
}

long double coshl(long double value) {
    long double positive = expl(fabsl(value));
    return (positive + 1.0L / positive) * 0.5L;
}

long double tanhl(long double value) {
    if (isnan(value) || value == 0.0L) return value;
    long double magnitude = fabsl(value);
    if (magnitude > 32.0L) return value < 0.0L ? -1.0L : 1.0L;
    long double scaled = expl(-2.0L * magnitude);
    long double result = (1.0L - scaled) / (1.0L + scaled);
    return value < 0.0L ? -result : result;
}

long double asinhl(long double value) {
    if (!isfinite(value) || value == 0.0L) return value;
    long double magnitude = fabsl(value);
    long double result = magnitude > x87_sqrt(LDBL_MAX) ?
                         logl(magnitude) + LN2 :
                         log1pl(magnitude +
                                magnitude * magnitude /
                                (1.0L + hypotl(magnitude, 1.0L)));
    return value < 0.0L ? -result : result;
}

long double acoshl(long double value) {
    if (value < 1.0L) return math_domain();
    if (!isfinite(value) || value == 1.0L) return value - 1.0L;
    if (value > x87_sqrt(LDBL_MAX)) return logl(value) + LN2;
    return logl(value + sqrtl(value - 1.0L) * sqrtl(value + 1.0L));
}

long double atanhl(long double value) {
    if (isnan(value) || value == 0.0L) return value;
    long double magnitude = fabsl(value);
    if (magnitude > 1.0L) return math_domain();
    if (magnitude == 1.0L) {
        errno = ERANGE;
        return copysignl(HUGE_VALL, value);
    }
    long double result = 0.5L * log1pl(2.0L * magnitude /
                                      (1.0L - magnitude));
    return value < 0.0L ? -result : result;
}

long double copysignl(long double magnitude, long double sign) {
    magnitude = fabsl(magnitude);
    return signbit(sign) ? -magnitude : magnitude;
}

long double fdiml(long double left, long double right) {
    if (isnan(left) || isnan(right)) return left + right;
    return left > right ? left - right : 0.0L;
}

long double fmaxl(long double left, long double right) {
    if (isnan(left)) return right;
    if (isnan(right)) return left;
    if (left == right) return signbit(left) ? right : left;
    return left > right ? left : right;
}

long double fminl(long double left, long double right) {
    if (isnan(left)) return right;
    if (isnan(right)) return left;
    if (left == right) return signbit(left) ? left : right;
    return left < right ? left : right;
}

typedef union {
    long double value;
    struct {
        unsigned long long significand;
        unsigned short sign_exponent;
        unsigned short padding[3];
    } parts;
} x87_value_t;

_Static_assert(sizeof(long double) == sizeof(x87_value_t),
               "System requires the x86 extended-precision ABI");

static void note_nextafter_range(int classification) {
    if (classification == FP_INFINITE || classification == FP_SUBNORMAL ||
        classification == FP_ZERO) errno = ERANGE;
}

long double nextafterl(long double value, long double toward) {
    if (isnan(value) || isnan(toward)) return value + toward;
    if (value == toward) return toward;

    x87_value_t bits = {.value = value};
    if (value == 0.0L) {
        bits.parts.significand = 1U;
        bits.parts.sign_exponent = signbit(toward) ? 0x8000U : 0U;
    } else {
        unsigned short sign = bits.parts.sign_exponent & 0x8000U;
        unsigned short exponent = bits.parts.sign_exponent & 0x7FFFU;
        bool increase_magnitude = (value < toward) == !signbit(value);
        if (increase_magnitude) {
            if (exponent == 0U) {
                ++bits.parts.significand;
                if (bits.parts.significand == 0x8000000000000000ULL) {
                    exponent = 1U;
                }
            } else if (bits.parts.significand == ~0ULL) {
                bits.parts.significand = 0x8000000000000000ULL;
                ++exponent;
            } else {
                ++bits.parts.significand;
            }
        } else if (exponent == 0U) {
            --bits.parts.significand;
        } else if (bits.parts.significand ==
                   0x8000000000000000ULL) {
            --exponent;
            bits.parts.significand = exponent == 0U ?
                0x7FFFFFFFFFFFFFFFULL : ~0ULL;
        } else {
            --bits.parts.significand;
        }
        bits.parts.sign_exponent = sign | exponent;
    }
    note_nextafter_range(fpclassify(bits.value));
    return bits.value;
}

long double nexttowardl(long double value, long double toward) {
    return nextafterl(value, toward);
}

long double nanl(const char *tag) {
    (void)tag;
    return __builtin_nanl("");
}

/* 在最终转换前，以 x87 扩展精度保留乘积和加数。这样无需依赖编译器运行时即可得到
 * 融合结果（否则显式调用 __builtin_fma 会递归回到此处）。 */
long double fmal(long double left, long double right, long double addend) {
    long double product;

    if ((isinf(left) && right == 0.0L) ||
        (isinf(right) && left == 0.0L)) {
        errno = EDOM;
        return __builtin_nanl("");
    }
    product = left * right;
    if (isinf(product) && isinf(addend) &&
        signbit(product) != signbit(addend)) {
        errno = EDOM;
        return __builtin_nanl("");
    }
    if (!isinf(left) && !isinf(right) && isinf(product + addend)) {
        errno = ERANGE;
    }
    return product + addend;
}

static long double erf_series(long double value) {
    long double square = value * value;
    long double term = value;
    long double sum = value;

    for (unsigned int index = 1U; index < 128U; ++index) {
        term *= -square / (long double)index;
        long double addend = term / (long double)(2U * index + 1U);
        sum += addend;
        if (fabsl(addend) <= fabsl(sum) * 0x1p-70L) break;
    }
    return 1.12837916709551257389615890312154517L * sum;
}

/* Numerical Recipes 中有界的互补误差函数近似算法。 */
static long double erfc_positive(long double value) {
    long double t;
    long double polynomial;

    if (value < 1.5L) return 1.0L - erf_series(value);
    t = 1.0L / (1.0L + 0.5L * value);
    polynomial = 0.17087277L;
    polynomial = -0.82215223L + t * polynomial;
    polynomial = 1.48851587L + t * polynomial;
    polynomial = -1.13520398L + t * polynomial;
    polynomial = 0.27886807L + t * polynomial;
    polynomial = -0.18628806L + t * polynomial;
    polynomial = 0.09678418L + t * polynomial;
    polynomial = 0.37409196L + t * polynomial;
    polynomial = 1.00002368L + t * polynomial;
    polynomial = -1.26551223L + t * polynomial;
    return t * expl(-value * value + polynomial);
}

long double erfl(long double value) {
    long double magnitude;
    long double result;

    if (isnan(value)) return value;
    if (isinf(value)) return signbit(value) ? -1.0L : 1.0L;
    magnitude = fabsl(value);
    if (magnitude < 1.5L) return erf_series(value);
    result = 1.0L - erfc_positive(magnitude);
    return signbit(value) ? -result : result;
}

long double erfcl(long double value) {
    if (isnan(value)) return value;
    if (value == HUGE_VALL) return 0.0L;
    if (value == -HUGE_VALL) return 2.0L;
    if (value < 0.0L) return 2.0L - erfc_positive(-value);
    return erfc_positive(value);
}

int signgam = 1;

static const long double g_lanczos[] = {
    0.99999999999980993227684700473478L,
    676.520368121885098567009190444019L,
    -1259.13921672240287047156078755283L,
    771.3234287776530788486528258894L,
    -176.61502916214059906584551354L,
    12.507343278686904814458936853L,
    -0.13857109526572011689554707L,
    9.984369578019570859563e-6L,
    1.50563273514931155834e-7L,
};

static bool gamma_negative_integer(long double value) {
    return value < 0.0L && value == truncl(value);
}

long double lgammal(long double value) {
    static const long double log_sqrt_two_pi =
        0.91893853320467274178032973640562L;
    long double sum;
    long double z;
    long double term;

    signgam = 1;
    if (isnan(value)) return value;
    if (isinf(value)) return value > 0.0L ? value : math_domain();
    if (value == 0.0L || gamma_negative_integer(value)) {
        errno = ERANGE;
        return HUGE_VALL;
    }
    if (value < 0.5L) {
        long double sine = sinl(PI * value);
        if (sine == 0.0L) {
            errno = ERANGE;
            return HUGE_VALL;
        }
        signgam = sine < 0.0L ? -1 : 1;
        return logl(PI) - logl(fabsl(sine)) - lgammal(1.0L - value);
    }

    z = value - 1.0L;
    sum = g_lanczos[0];
    for (unsigned int index = 1U; index < sizeof(g_lanczos) /
                                  sizeof(g_lanczos[0]); ++index) {
        term = z + (long double)index;
        sum += g_lanczos[index] / term;
    }
    term = z + 7.5L;
    return log_sqrt_two_pi + (z + 0.5L) * logl(term) - term + logl(sum);
}

long double tgammal(long double value) {
    long double logarithm = lgammal(value);
    long double result;

    if (isnan(logarithm)) return logarithm;
    if (isinf(logarithm)) return signgam < 0 ? -HUGE_VALL : HUGE_VALL;
    result = expl(logarithm);
    return signgam < 0 ? -result : result;
}

#define DEFINE_UNARY(name) \
    __attribute__((target("sse"))) float name##f(float value) { \
        return (float)name##l((long double)value); \
    } \
    __attribute__((target("sse"))) double name(double value) { \
        return (double)name##l((long double)value); \
    }

#define DEFINE_BINARY(name) \
    __attribute__((target("sse"))) float name##f(float left, float right) { \
        return (float)name##l((long double)left, (long double)right); \
    } \
    __attribute__((target("sse"))) double name(double left, double right) { \
        return (double)name##l((long double)left, (long double)right); \
    }

#define DEFINE_TERNARY(name) \
    __attribute__((target("sse"))) float name##f(float first, float second, \
                                                   float third) { \
        return (float)name##l((long double)first, (long double)second, \
                              (long double)third); \
    } \
    __attribute__((target("sse"))) double name(double first, double second, \
                                                 double third) { \
        return (double)name##l((long double)first, (long double)second, \
                               (long double)third); \
    }

DEFINE_UNARY(fabs)
DEFINE_UNARY(sqrt)
DEFINE_UNARY(cbrt)
DEFINE_UNARY(sin)
DEFINE_UNARY(cos)

__attribute__((target("sse"))) void sincosf(float value, float *sine,
                                               float *cosine) {
    if (sine != 0) *sine = sinf(value);
    if (cosine != 0) *cosine = cosf(value);
}

void sincos(double value, double *sine, double *cosine) {
    if (sine != 0) *sine = sin(value);
    if (cosine != 0) *cosine = cos(value);
}

void sincosl(long double value, long double *sine, long double *cosine) {
    if (sine != 0) *sine = sinl(value);
    if (cosine != 0) *cosine = cosl(value);
}

DEFINE_UNARY(tan)
DEFINE_UNARY(asin)
DEFINE_UNARY(acos)
DEFINE_UNARY(atan)
DEFINE_BINARY(atan2)
DEFINE_UNARY(sinh)
DEFINE_UNARY(cosh)
DEFINE_UNARY(tanh)
DEFINE_UNARY(asinh)
DEFINE_UNARY(acosh)
DEFINE_UNARY(atanh)
DEFINE_UNARY(exp)
DEFINE_UNARY(exp2)
DEFINE_UNARY(expm1)
DEFINE_UNARY(log)
DEFINE_UNARY(log2)
DEFINE_UNARY(log10)
DEFINE_UNARY(log1p)
DEFINE_BINARY(pow)
DEFINE_TERNARY(fma)
DEFINE_UNARY(erf)
DEFINE_UNARY(erfc)
DEFINE_UNARY(lgamma)
DEFINE_UNARY(tgamma)
DEFINE_BINARY(hypot)
DEFINE_UNARY(ceil)
DEFINE_UNARY(floor)
DEFINE_UNARY(trunc)
DEFINE_UNARY(round)
DEFINE_UNARY(rint)
DEFINE_UNARY(nearbyint)
DEFINE_UNARY(logb)
DEFINE_BINARY(fmod)
DEFINE_BINARY(remainder)
DEFINE_BINARY(copysign)
DEFINE_BINARY(fdim)
DEFINE_BINARY(fmax)
DEFINE_BINARY(fmin)

__attribute__((target("sse"))) float frexpf(float value, int *exponent) {
    return (float)frexpl((long double)value, exponent);
}

__attribute__((target("sse"))) double frexp(double value, int *exponent) {
    return (double)frexpl((long double)value, exponent);
}

__attribute__((target("sse"))) float modff(float value, float *integer) {
    long double wide_integer;
    long double result = modfl((long double)value, &wide_integer);
    *integer = (float)wide_integer;
    return (float)result;
}

__attribute__((target("sse"))) double modf(double value, double *integer) {
    long double wide_integer;
    long double result = modfl((long double)value, &wide_integer);
    *integer = (double)wide_integer;
    return (double)result;
}

#define DEFINE_INTEGER_UNARY(name, result_type) \
    __attribute__((target("sse"))) result_type name##f(float value) { \
        return name##l((long double)value); \
    } \
    __attribute__((target("sse"))) result_type name(double value) { \
        return name##l((long double)value); \
    }

DEFINE_INTEGER_UNARY(ilogb, int)
DEFINE_INTEGER_UNARY(lrint, long)
DEFINE_INTEGER_UNARY(llrint, long long)
DEFINE_INTEGER_UNARY(lround, long)
DEFINE_INTEGER_UNARY(llround, long long)

__attribute__((target("sse"))) float scalbnf(float value, int exponent) {
    return (float)scalbnl((long double)value, exponent);
}

__attribute__((target("sse"))) double scalbn(double value, int exponent) {
    return (double)scalbnl((long double)value, exponent);
}

__attribute__((target("sse"))) float scalblnf(float value, long exponent) {
    return (float)scalblnl((long double)value, exponent);
}

__attribute__((target("sse"))) double scalbln(double value, long exponent) {
    return (double)scalblnl((long double)value, exponent);
}

__attribute__((target("sse"))) float ldexpf(float value, int exponent) {
    return scalbnf(value, exponent);
}

__attribute__((target("sse"))) double ldexp(double value, int exponent) {
    return scalbn(value, exponent);
}

__attribute__((target("sse"))) float remquof(float left, float right,
                                              int *quotient) {
    return (float)remquol((long double)left, (long double)right, quotient);
}

__attribute__((target("sse"))) double remquo(double left, double right,
                                              int *quotient) {
    return (double)remquol((long double)left, (long double)right, quotient);
}

__attribute__((target("sse"))) float nextafterf(float value, float toward) {
    if (isnan(value) || isnan(toward)) return value + toward;
    if (value == toward) return toward;
    union {
        float value;
        unsigned int bits;
    } representation = {.value = value};
    if (value == 0.0F) {
        representation.bits = signbit(toward) ? 0x80000001U : 1U;
    } else if ((value < toward) == (value > 0.0F)) {
        ++representation.bits;
    } else {
        --representation.bits;
    }
    note_nextafter_range(fpclassify(representation.value));
    return representation.value;
}

__attribute__((target("sse"))) double nextafter(double value, double toward) {
    if (isnan(value) || isnan(toward)) return value + toward;
    if (value == toward) return toward;
    union {
        double value;
        unsigned long long bits;
    } representation = {.value = value};
    if (value == 0.0) {
        representation.bits = signbit(toward) ?
            0x8000000000000001ULL : 1ULL;
    } else if ((value < toward) == (value > 0.0)) {
        ++representation.bits;
    } else {
        --representation.bits;
    }
    note_nextafter_range(fpclassify(representation.value));
    return representation.value;
}

__attribute__((target("sse"))) float nexttowardf(float value,
                                                  long double toward) {
    if (isnan(value) || isnan(toward)) return (float)((long double)value + toward);
    if ((long double)value == toward) return (float)toward;
    return nextafterf(value, (long double)value < toward ? HUGE_VALF : -HUGE_VALF);
}

__attribute__((target("sse"))) double nexttoward(double value,
                                                  long double toward) {
    if (isnan(value) || isnan(toward)) return (double)((long double)value + toward);
    if ((long double)value == toward) return (double)toward;
    return nextafter(value, (long double)value < toward ? HUGE_VAL : -HUGE_VAL);
}

__attribute__((target("sse"))) float nanf(const char *tag) {
    return (float)nanl(tag);
}

__attribute__((target("sse"))) double nan(const char *tag) {
    return (double)nanl(tag);
}

#undef DEFINE_INTEGER_UNARY
#undef DEFINE_TERNARY
#undef DEFINE_UNARY
#undef DEFINE_BINARY
