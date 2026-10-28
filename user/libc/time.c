#include "libc/libc.h"

#include <limits.h>
#include <sys/time.h>
#include <time.h>

#define SECONDS_PER_MINUTE 60LL
#define SECONDS_PER_HOUR   (60LL * SECONDS_PER_MINUTE)
#define SECONDS_PER_DAY    (24LL * SECONDS_PER_HOUR)

char *tzname[2] = {"UTC", "UTC"};

static int64_t floor_divide(int64_t value, int64_t divisor) {
    int64_t quotient = value / divisor;
    int64_t remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

static int64_t positive_modulo(int64_t value, int64_t divisor) {
    int64_t result = value % divisor;
    return result < 0 ? result + divisor : result;
}

static bool checked_add_i64(int64_t left, int64_t right, int64_t *result) {
    if (result == 0 || (right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right)) return false;
    *result = left + right;
    return true;
}

static bool checked_mul_i64(int64_t left, int64_t right, int64_t *result) {
    if (result == 0 || left == 0 || right == 0) {
        if (result != 0) *result = 0;
        return result != 0;
    }
    if (left == -1 && right == INT64_MIN) return false;
    if (right == -1 && left == INT64_MIN) return false;
    if (left > 0) {
        if (right > 0 && left > INT64_MAX / right) return false;
        if (right < 0 && right < INT64_MIN / left) return false;
    } else {
        if (right > 0 && left < INT64_MIN / right) return false;
        if (right < 0 && left < INT64_MAX / right) return false;
    }
    *result = left * right;
    return true;
}

/* Howard Hinnant 的民用日历转换算法。仅使用整数运算，避免运行时依赖浮点辅助库。 */
static int64_t days_from_civil(int64_t year, unsigned int month,
                               unsigned int day) {
    int64_t adjusted_year = year - (month <= 2U ? 1 : 0);
    int64_t era = floor_divide(adjusted_year, 400);
    unsigned int year_of_era = (unsigned int)(adjusted_year - era * 400);
    unsigned int month_index = month + (month > 2U ? (unsigned int)-3 : 9U);
    unsigned int day_of_year = (153U * month_index + 2U) / 5U + day - 1U;
    unsigned int day_of_era = year_of_era * 365U + year_of_era / 4U -
                              year_of_era / 100U + day_of_year;
    return era * 146097 + (int64_t)day_of_era - 719468;
}

static void civil_from_days(int64_t days, int64_t *year,
                            unsigned int *month, unsigned int *day) {
    int64_t shifted = days + 719468;
    int64_t era = floor_divide(shifted, 146097);
    unsigned int day_of_era =
        (unsigned int)(shifted - era * 146097);
    unsigned int year_of_era =
        (day_of_era - day_of_era / 1460U + day_of_era / 36524U -
         day_of_era / 146096U) / 365U;
    int64_t resolved_year = era * 400 + (int64_t)year_of_era;
    unsigned int day_of_year = day_of_era -
        (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
    unsigned int month_index = (5U * day_of_year + 2U) / 153U;
    *year = resolved_year + (month_index < 10U ? 0 : 1);
    *month = month_index + (month_index < 10U ? 3U : (unsigned int)-9);
    *day = day_of_year - (153U * month_index + 2U) / 5U + 1U;
}

struct tm *gmtime_r(const time_t *value, struct tm *result) {
    int64_t days;
    int64_t seconds;
    int64_t year;
    unsigned int month;
    unsigned int day;
    if (value == 0 || result == 0) {
        errno = EINVAL;
        return 0;
    }
    days = floor_divide(*value, SECONDS_PER_DAY);
    seconds = *value - days * SECONDS_PER_DAY;
    civil_from_days(days, &year, &month, &day);
    if (year < (int64_t)INT_MIN + 1900LL ||
        year > (int64_t)INT_MAX + 1900LL) {
        errno = EOVERFLOW;
        return 0;
    }
    result->tm_year = (int)(year - 1900LL);
    result->tm_mon = (int)month - 1;
    result->tm_mday = (int)day;
    result->tm_hour = (int)(seconds / SECONDS_PER_HOUR);
    seconds %= SECONDS_PER_HOUR;
    result->tm_min = (int)(seconds / SECONDS_PER_MINUTE);
    result->tm_sec = (int)(seconds % SECONDS_PER_MINUTE);
    result->tm_wday = (int)positive_modulo(days + 4, 7);
    result->tm_yday = (int)(days - days_from_civil(year, 1U, 1U));
    result->tm_isdst = 0;
    result->tm_gmtoff = 0;
    result->tm_zone = tzname[0];
    return result;
}

struct tm *localtime_r(const time_t *value, struct tm *result) {
    /* System 当前没有时区数据库；本地时间使用 UTC。 */
    return gmtime_r(value, result);
}

struct tm *gmtime(const time_t *value) {
    static struct tm result;
    return gmtime_r(value, &result);
}

struct tm *localtime(const time_t *value) {
    static struct tm result;
    return localtime_r(value, &result);
}

time_t time(time_t *result) {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) < 0) return (time_t)-1;
    if (result != 0) *result = now.tv_sec;
    return now.tv_sec;
}

clock_t clock(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return (clock_t)-1;
    if (now.tv_sec > INT64_MAX / 1000000LL) return (clock_t)-1;
    return (clock_t)(now.tv_sec * 1000000LL + now.tv_nsec / 1000L);
}

int clock_getres(int clock_id, struct timespec *value) {
    if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC) {
        errno = EINVAL;
        return -1;
    }
    if (value != 0) {
        value->tv_sec = 0;
        value->tv_nsec = 1;
    }
    return 0;
}

int timespec_get(struct timespec *value, int base) {
    if (value == 0 || base != TIME_UTC) {
        errno = EINVAL;
        return 0;
    }
    return clock_gettime(CLOCK_REALTIME, value) == 0 ? base : 0;
}

void tzset(void) {
    /* System 目前仅提供 UTC，且没有时区数据库。 */
}

__attribute__((target("sse"))) double difftime(time_t end, time_t start) {
    return (double)(end - start);
}

time_t mktime(struct tm *value) {
    int64_t year;
    int64_t month_index;
    int64_t days;
    int64_t day_offset;
    int64_t day_seconds;
    int64_t seconds;
    time_t result;
    if (value == 0) {
        errno = EINVAL;
        return (time_t)-1;
    }
    year = (int64_t)value->tm_year + 1900LL;
    /* 先规范化月份，使负数或超范围月份在民用日历转换前进位到年份。 */
    month_index = floor_divide((int64_t)value->tm_mon, 12LL);
    if (!checked_add_i64(year, month_index, &year)) {
        errno = EOVERFLOW;
        return (time_t)-1;
    }
    month_index = positive_modulo((int64_t)value->tm_mon, 12LL);
    days = days_from_civil(year, (unsigned int)month_index + 1U, 1U);
    if (!checked_add_i64(days, (int64_t)value->tm_mday - 1LL, &days)) {
        errno = EOVERFLOW;
        return (time_t)-1;
    }
    day_seconds = (int64_t)value->tm_hour * SECONDS_PER_HOUR +
                  (int64_t)value->tm_min * SECONDS_PER_MINUTE +
                  (int64_t)value->tm_sec;
    day_offset = floor_divide(day_seconds, SECONDS_PER_DAY);
    day_seconds = positive_modulo(day_seconds, SECONDS_PER_DAY);
    if (!checked_add_i64(days, day_offset, &days) ||
        !checked_mul_i64(days, SECONDS_PER_DAY, &seconds) ||
        !checked_add_i64(seconds, day_seconds, &seconds)) {
        errno = EOVERFLOW;
        return (time_t)-1;
    }
    result = (time_t)seconds;
    if (gmtime_r(&result, value) == 0) return (time_t)-1;
    return result;
}

static size_t append_character(char *buffer, size_t capacity, size_t length,
                               char value) {
    if (length + 1U < capacity) buffer[length] = value;
    return length + 1U;
}

static size_t append_text(char *buffer, size_t capacity, size_t length,
                          const char *text) {
    if (text == 0) text = "";
    while (*text != '\0') length = append_character(buffer, capacity, length,
                                                     *text++);
    return length;
}

static size_t append_unsigned(char *buffer, size_t capacity, size_t length,
                              unsigned int value, unsigned int width) {
    char digits[16];
    unsigned int count = 0U;
    while (value != 0U || count == 0U) {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    }
    while (count < width) {
        length = append_character(buffer, capacity, length, '0');
        --width;
    }
    while (count != 0U) length = append_character(buffer, capacity, length,
                                                   digits[--count]);
    return length;
}

static size_t append_signed_year(char *buffer, size_t capacity, size_t length,
                                 int value) {
    if (value < 0) {
        length = append_character(buffer, capacity, length, '-');
        return append_unsigned(buffer, capacity, length,
                               (unsigned int)(-(value + 1)) + 1U, 4U);
    }
    return append_unsigned(buffer, capacity, length, (unsigned int)value, 4U);
}

static size_t append_weekday_name(char *buffer, size_t capacity, size_t length,
                                  const struct tm *value, bool long_name) {
    static const char *const short_names[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const long_names[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
        "Friday", "Saturday"};
    int index = value->tm_wday;
    if (index < 0 || index > 6) index = 0;
    return append_text(buffer, capacity, length,
                       (long_name ? long_names : short_names)[index]);
}

static size_t append_month_name(char *buffer, size_t capacity, size_t length,
                                const struct tm *value, bool long_name) {
    static const char *const short_names[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static const char *const long_names[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"};
    int index = value->tm_mon;
    if (index < 0 || index > 11) index = 0;
    return append_text(buffer, capacity, length,
                       (long_name ? long_names : short_names)[index]);
}

size_t strftime(char *buffer, size_t capacity, const char *format,
                const struct tm *value) {
    size_t length = 0U;
    if (buffer == 0 || capacity == 0U || format == 0 || value == 0) {
        errno = EINVAL;
        return 0U;
    }
    while (*format != '\0') {
        if (*format++ != '%') {
            length = append_character(buffer, capacity, length, format[-1]);
            continue;
        }
        switch (*format == '\0' ? '%' : *format++) {
        case '%': length = append_character(buffer, capacity, length, '%'); break;
        case 'a': length = append_weekday_name(buffer, capacity, length, value, false); break;
        case 'A': length = append_weekday_name(buffer, capacity, length, value, true); break;
        case 'b':
        case 'h': length = append_month_name(buffer, capacity, length, value, false); break;
        case 'B': length = append_month_name(buffer, capacity, length, value, true); break;
        case 'c':
            length = append_weekday_name(buffer, capacity, length, value, false);
            length = append_text(buffer, capacity, length, " ");
            length = append_month_name(buffer, capacity, length, value, false);
            length = append_text(buffer, capacity, length, " ");
            length = append_unsigned(buffer, capacity, length,
                                     (unsigned int)value->tm_mday, 2U);
            length = append_text(buffer, capacity, length, " ");
            length = append_unsigned(buffer, capacity, length,
                                     (unsigned int)value->tm_hour, 2U);
            length = append_character(buffer, capacity, length, ':');
            length = append_unsigned(buffer, capacity, length,
                                     (unsigned int)value->tm_min, 2U);
            length = append_character(buffer, capacity, length, ':');
            length = append_unsigned(buffer, capacity, length,
                                     (unsigned int)value->tm_sec, 2U);
            length = append_text(buffer, capacity, length, " ");
            length = append_signed_year(buffer, capacity, length,
                                        value->tm_year + 1900);
            break;
        case 'C': length = append_unsigned(buffer, capacity, length,
                                           (unsigned int)((value->tm_year + 1900) / 100), 2U); break;
        case 'd': length = append_unsigned(buffer, capacity, length,
                                           (unsigned int)value->tm_mday, 2U); break;
        case 'e': {
            unsigned int day = (unsigned int)value->tm_mday;
            if (day < 10U) length = append_character(buffer, capacity, length, ' ');
            length = append_unsigned(buffer, capacity, length, day, 1U);
            break;
        }
        case 'H': length = append_unsigned(buffer, capacity, length,
                                           (unsigned int)value->tm_hour, 2U); break;
        case 'I': {
            unsigned int hour = (unsigned int)value->tm_hour % 12U;
            length = append_unsigned(buffer, capacity, length, hour == 0U ? 12U : hour, 2U);
            break;
        }
        case 'j': length = append_unsigned(buffer, capacity, length,
                                           (unsigned int)value->tm_yday + 1U, 3U); break;
        case 'm': length = append_unsigned(buffer, capacity, length,
                                           (unsigned int)value->tm_mon + 1U, 2U); break;
        case 'M': length = append_unsigned(buffer, capacity, length,
                                           (unsigned int)value->tm_min, 2U); break;
        case 'n': length = append_character(buffer, capacity, length, '\n'); break;
        case 'p': length = append_text(buffer, capacity, length,
                                       value->tm_hour < 12 ? "AM" : "PM"); break;
        case 'r':
            length = append_unsigned(buffer, capacity, length,
                                     (unsigned int)(value->tm_hour % 12 == 0 ? 12 : value->tm_hour % 12), 2U);
            length = append_text(buffer, capacity, length, ":");
            length = append_unsigned(buffer, capacity, length, (unsigned int)value->tm_min, 2U);
            length = append_text(buffer, capacity, length, ":");
            length = append_unsigned(buffer, capacity, length, (unsigned int)value->tm_sec, 2U);
            length = append_text(buffer, capacity, length, value->tm_hour < 12 ? " AM" : " PM");
            break;
        case 'S': length = append_unsigned(buffer, capacity, length,
                                           (unsigned int)value->tm_sec, 2U); break;
        case 't': length = append_character(buffer, capacity, length, '\t'); break;
        case 'u': {
            int weekday = value->tm_wday == 0 ? 7 : value->tm_wday;
            length = append_unsigned(buffer, capacity, length, (unsigned int)weekday, 1U);
            break;
        }
        case 'w': length = append_unsigned(buffer, capacity, length,
                                           (unsigned int)value->tm_wday, 1U); break;
        case 'x':
            length = append_unsigned(buffer, capacity, length,
                                     (unsigned int)value->tm_mon + 1U, 2U);
            length = append_character(buffer, capacity, length, '/');
            length = append_unsigned(buffer, capacity, length,
                                     (unsigned int)value->tm_mday, 2U);
            length = append_character(buffer, capacity, length, '/');
            length = append_unsigned(buffer, capacity, length,
                                     (unsigned int)((value->tm_year + 1900) % 100), 2U);
            break;
        case 'X':
            length = append_unsigned(buffer, capacity, length,
                                     (unsigned int)value->tm_hour, 2U);
            length = append_character(buffer, capacity, length, ':');
            length = append_unsigned(buffer, capacity, length,
                                     (unsigned int)value->tm_min, 2U);
            length = append_character(buffer, capacity, length, ':');
            length = append_unsigned(buffer, capacity, length,
                                     (unsigned int)value->tm_sec, 2U);
            break;
        case 'y': length = append_unsigned(buffer, capacity, length,
                                           (unsigned int)((value->tm_year + 1900) % 100), 2U); break;
        case 'Y': length = append_signed_year(buffer, capacity, length,
                                              value->tm_year + 1900); break;
        case 'z': length = append_text(buffer, capacity, length, "+0000"); break;
        case 'Z': length = append_text(buffer, capacity, length, "UTC"); break;
        default:
            errno = EINVAL;
            if (capacity != 0U) buffer[0] = '\0';
            return 0U;
        }
        if (length >= capacity) {
            buffer[capacity - 1U] = '\0';
            return 0U;
        }
    }
    buffer[length] = '\0';
    return length;
}

char *asctime_r(const struct tm *value, char *buffer) {
    size_t length;
    if (value == 0 || buffer == 0) {
        errno = EINVAL;
        return 0;
    }
    length = strftime(buffer, 26U, "%a %b %e %H:%M:%S %Y\n", value);
    return length == 25U ? buffer : 0;
}

char *asctime(const struct tm *value) {
    static char buffer[26];
    return asctime_r(value, buffer);
}

char *ctime_r(const time_t *value, char *buffer) {
    struct tm converted;
    if (gmtime_r(value, &converted) == 0) return 0;
    return asctime_r(&converted, buffer);
}

char *ctime(const time_t *value) {
    static char buffer[26];
    return ctime_r(value, buffer);
}

int gettimeofday(struct timeval *value, struct timezone *zone) {
    struct timespec now;
    if (value == 0) {
        errno = EINVAL;
        return -1;
    }
    if (clock_gettime(CLOCK_REALTIME, &now) < 0) return -1;
    value->tv_sec = now.tv_sec;
    value->tv_usec = now.tv_nsec / 1000L;
    if (zone != 0) {
        zone->tz_minuteswest = 0;
        zone->tz_dsttime = 0;
    }
    return 0;
}

int settimeofday(const struct timeval *value, const struct timezone *zone) {
    (void)zone;
    struct timespec converted;
    if (value == 0 || value->tv_usec < 0 || value->tv_usec >= 1000000L) {
        errno = EINVAL;
        return -1;
    }
    converted.tv_sec = value->tv_sec;
    converted.tv_nsec = value->tv_usec * 1000L;
    return clock_settime(CLOCK_REALTIME, &converted);
}

int utimes(const char *path, const struct timeval times[2]) {
    (void)path;
    (void)times;
    errno = ENOSYS;
    return -1;
}

int futimes(int descriptor, const struct timeval times[2]) {
    (void)descriptor;
    (void)times;
    errno = ENOSYS;
    return -1;
}

int clock_nanosleep(clockid_t clock_id, int flags,
                    const struct timespec *request,
                    struct timespec *remaining) {
    struct timespec relative;
    struct timespec now;
    if ((clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC) ||
        (flags & ~TIMER_ABSTIME) != 0 || request == 0 ||
        request->tv_sec < 0 || request->tv_nsec < 0 ||
        request->tv_nsec >= 1000000000L) {
        return EINVAL;
    }
    if ((flags & TIMER_ABSTIME) == 0) {
        return nanosleep(request, remaining) == 0 ? 0 : errno;
    }
    if (clock_gettime(clock_id, &now) < 0) return errno;
    if (request->tv_sec < now.tv_sec ||
        (request->tv_sec == now.tv_sec && request->tv_nsec <= now.tv_nsec)) {
        if (remaining != 0) *remaining = (struct timespec){0, 0};
        return 0;
    }
    relative.tv_sec = request->tv_sec - now.tv_sec;
    if (request->tv_nsec < now.tv_nsec) {
        --relative.tv_sec;
        relative.tv_nsec = request->tv_nsec + 1000000000L - now.tv_nsec;
    } else {
        relative.tv_nsec = request->tv_nsec - now.tv_nsec;
    }
    return nanosleep(&relative, remaining) == 0 ? 0 : errno;
}
