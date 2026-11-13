#pragma once

#include <libc/libc.h>
#include <sys/types.h>

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;
    char *tm_zone;
};

typedef int clockid_t;
#ifndef CLOCKS_PER_SEC
#define CLOCKS_PER_SEC 1000000L
#endif
#ifndef _POSIX_TIMERS
#define _POSIX_TIMERS 200809L
#endif
#ifndef _POSIX_MONOTONIC_CLOCK
#define _POSIX_MONOTONIC_CLOCK 0
#endif
#ifndef _POSIX_THREAD_CPUTIME
#define _POSIX_THREAD_CPUTIME 0
#endif
#define CLOCK_REALTIME ((clockid_t)OS_CLOCK_REALTIME)
#define CLOCK_MONOTONIC ((clockid_t)OS_CLOCK_MONOTONIC)
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME CLOCK_MONOTONIC
#endif
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif
#ifndef CLOCK_REALTIME_COARSE
#define CLOCK_REALTIME_COARSE CLOCK_REALTIME
#endif
#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC
#endif
#ifndef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID CLOCK_MONOTONIC
#endif
#ifndef CLOCK_PROCESS_CPUTIME_ID
#define CLOCK_PROCESS_CPUTIME_ID CLOCK_MONOTONIC
#endif
#define TIME_UTC 1
#define TIMER_ABSTIME 1

struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};

/* System 当前只公开 UTC 时区，但 POSIX/ICU 仍要求存在传统的时区名称数组。 */
#ifdef __cplusplus
extern "C" {
#endif
extern char *tzname[2];

time_t time(time_t *result);
clock_t clock(void);
int clock_gettime(clockid_t clock_id, struct timespec *value);
int clock_settime(clockid_t clock_id, const struct timespec *value);
int clock_getres(clockid_t clock_id, struct timespec *value);
int timespec_get(struct timespec *value, int base);
int nanosleep(const struct timespec *request, struct timespec *remaining);
int clock_nanosleep(clockid_t clock_id, int flags,
                    const struct timespec *request,
                    struct timespec *remaining);
void tzset(void);
double difftime(time_t end, time_t start);
struct tm *gmtime(const time_t *value);
struct tm *localtime(const time_t *value);
struct tm *gmtime_r(const time_t *value, struct tm *result);
struct tm *localtime_r(const time_t *value, struct tm *result);
time_t mktime(struct tm *value);
time_t timegm(struct tm *value);
char *asctime(const struct tm *value);
char *asctime_r(const struct tm *value, char *buffer);
char *ctime(const time_t *value);
char *ctime_r(const time_t *value, char *buffer);
size_t strftime(char *buffer, size_t capacity, const char *format,
                const struct tm *value);
char *strptime(const char *text, const char *format, struct tm *value);

#ifdef __cplusplus
}
#endif
