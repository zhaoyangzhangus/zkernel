#pragma once

#include <libc/libc.h>
#include <stdint.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647
#define MB_CUR_MAX 4

/* 编译器为此 freestanding 目标提供 alloca；公开常见的 libc 名称，
 * 无需引入宿主的 alloca.h。 */
#ifndef alloca
#define alloca(size) __builtin_alloca(size)
#endif

/* glibc 公开这些进程名称指针，Chromium 更新标题时会使用短名称槽位。 */
extern char *program_invocation_name;
extern char *program_invocation_short_name;

#ifdef __cplusplus
extern "C" {
#endif
__attribute__((target("sse"))) double atof(const char *text);
unsigned long strtoul(const char *text, char **end, int base);
long strtol(const char *text, char **end, int base);
long long strtoll(const char *text, char **end, int base);
unsigned long long strtoull(const char *text, char **end, int base);
intmax_t strtoimax(const char *text, char **end, int base);
uintmax_t strtoumax(const char *text, char **end, int base);
double strtod(const char *text, char **end);
float strtof(const char *text, char **end);
long double strtold(const char *text, char **end);
int abs(int value);
long labs(long value);
long long llabs(long long value);
div_t div(int numerator, int denominator);
ldiv_t ldiv(long numerator, long denominator);
lldiv_t lldiv(long long numerator, long long denominator);
void srand(unsigned int seed);
int rand(void);
int rand_r(unsigned int *seed);

/* fontconfig 使用的可重入 BSD/glibc 风格随机状态。System 刻意保持状态结构精简；
 * 调用方只依赖 initstate_r/random_r 的文档约定，不依赖 glibc 内部的多项式字段。 */
struct random_data {
    unsigned int state;
};
int initstate_r(unsigned int seed, char *state_buffer, size_t state_length,
               struct random_data *buffer);
int random_r(struct random_data *buffer, int32_t *result);

void qsort(void *base, size_t count, size_t size,
          int (*compare)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *));
char *getenv(const char *name);
char *secure_getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *assignment);
int atexit(void (*function)(void));
int at_quick_exit(void (*function)(void));
void quick_exit(int status) __attribute__((noreturn));
int system(const char *command);
int mkstemp(char *template_name);
char *mkdtemp(char *template_name);
char *mktemp(char *template_name);
char *realpath(const char *path, char *resolved);
int getsubopt(char **optionp, char *const *tokens, char **valuep);
#ifdef __cplusplus
}
#endif
