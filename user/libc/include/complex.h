#pragma once

#include <math.h>

/* GCC 直接公开 C99 复数表示。使用宏构造复数，使每个参数恰好求值一次，
 * 并保留其声明精度。 */
#define complex _Complex
#define _Complex_I (__extension__ 1.0fi)
#define I _Complex_I

#define CMPLXF(real, imaginary) \
    __builtin_complex((float)(real), (float)(imaginary))
#define CMPLX(real, imaginary) \
    __builtin_complex((double)(real), (double)(imaginary))
#define CMPLXL(real, imaginary) \
    __builtin_complex((long double)(real), (long double)(imaginary))

float crealf(float complex value);
double creal(double complex value);
long double creall(long double complex value);
float cimagf(float complex value);
double cimag(double complex value);
long double cimagl(long double complex value);

float cabsf(float complex value);
double cabs(double complex value);
long double cabsl(long double complex value);
float cargf(float complex value);
double carg(double complex value);
long double cargl(long double complex value);

float complex conjf(float complex value);
double complex conj(double complex value);
long double complex conjl(long double complex value);
float complex cprojf(float complex value);
double complex cproj(double complex value);
long double complex cprojl(long double complex value);

float complex cexpf(float complex value);
double complex cexp(double complex value);
long double complex cexpl(long double complex value);
float complex clogf(float complex value);
double complex clog(double complex value);
long double complex clogl(long double complex value);
float complex cpowf(float complex base, float complex exponent);
double complex cpow(double complex base, double complex exponent);
long double complex cpowl(long double complex base, long double complex exponent);
float complex csqrtf(float complex value);
double complex csqrt(double complex value);
long double complex csqrtl(long double complex value);

float complex csinf(float complex value);
double complex csin(double complex value);
long double complex csinl(long double complex value);
float complex ccosf(float complex value);
double complex ccos(double complex value);
long double complex ccosl(long double complex value);
float complex ctanf(float complex value);
double complex ctan(double complex value);
long double complex ctanl(long double complex value);
float complex csinhf(float complex value);
double complex csinh(double complex value);
long double complex csinhl(long double complex value);
float complex ccoshf(float complex value);
double complex ccosh(double complex value);
long double complex ccoshl(long double complex value);
float complex ctanhf(float complex value);
double complex ctanh(double complex value);
long double complex ctanhl(long double complex value);

float complex casinf(float complex value);
double complex casin(double complex value);
long double complex casinl(long double complex value);
float complex cacosf(float complex value);
double complex cacos(double complex value);
long double complex cacosl(long double complex value);
float complex catanf(float complex value);
double complex catan(double complex value);
long double complex catanl(long double complex value);
float complex casinhf(float complex value);
double complex casinh(double complex value);
long double complex casinhl(long double complex value);
float complex cacoshf(float complex value);
double complex cacosh(double complex value);
long double complex cacoshl(long double complex value);
float complex catanhf(float complex value);
double complex catanh(double complex value);
long double complex catanhl(long double complex value);
