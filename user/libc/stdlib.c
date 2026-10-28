#include "libc/libc.h"

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

char *program_invocation_name;
char *program_invocation_short_name;

static bool digit_value(int value, unsigned int *digit) {
    if (value >= '0' && value <= '9') *digit = (unsigned int)(value - '0');
    else if (value >= 'a' && value <= 'z') *digit = (unsigned int)(value - 'a') + 10U;
    else if (value >= 'A' && value <= 'Z') *digit = (unsigned int)(value - 'A') + 10U;
    else return false;
    return true;
}

static const char *skip_space(const char *text) {
    if (text == 0) return 0;
    while (isspace((unsigned char)*text)) ++text;
    return text;
}

static const char *scan_base_prefix(const char *text, int *base) {
    if (*base == 0) {
        *base = 10;
        if (text[0] == '0') {
            if ((text[1] == 'x' || text[1] == 'X') &&
                isxdigit((unsigned char)text[2])) {
                *base = 16;
                return text + 2;
            }
            *base = 8;
        }
    } else if (*base == 16 && text[0] == '0' &&
               (text[1] == 'x' || text[1] == 'X') &&
               isxdigit((unsigned char)text[2])) {
        return text + 2;
    }
    return text;
}

static unsigned long long parse_unsigned(const char *text, char **end,
                                         int base, unsigned long long limit,
                                         bool *negative, bool *converted,
                                         bool *overflowed) {
    const char *cursor;
    const char *digits_start;
    unsigned long long result = 0U;
    bool overflow = false;
    unsigned int digit;

    if (end != 0) *end = (char *)text;
    if (converted != 0) *converted = false;
    if (negative != 0) *negative = false;
    if (overflowed != 0) *overflowed = false;
    if (text == 0) {
        errno = EINVAL;
        return 0U;
    }
    cursor = skip_space(text);
    if (*cursor == '+' || *cursor == '-') {
        if (negative != 0) *negative = *cursor == '-';
        ++cursor;
    }
    if (base < 0 || base == 1 || base > 36) {
        errno = EINVAL;
        return 0U;
    }
    cursor = scan_base_prefix(cursor, &base);
    digits_start = cursor;
    while (digit_value((unsigned char)*cursor, &digit) && digit < (unsigned int)base) {
        if (result > (limit - digit) / (unsigned int)base) {
            result = limit;
            overflow = true;
        } else if (!overflow) {
            result = result * (unsigned int)base + digit;
        }
        ++cursor;
    }
    if (cursor == digits_start) {
        if (end != 0) *end = (char *)text;
        return 0U;
    }
    if (converted != 0) *converted = true;
    if (end != 0) *end = (char *)cursor;
    if (overflow) errno = ERANGE;
    if (overflowed != 0) *overflowed = overflow;
    return result;
}

long strtol(const char *text, char **end, int base) {
    bool negative;
    bool converted;
    bool overflow;
    unsigned long long limit = (unsigned long long)LONG_MAX;
    unsigned long long value;
    if (text == 0) {
        if (end != 0) *end = 0;
        errno = EINVAL;
        return 0;
    }
    value = parse_unsigned(text, end, base,
                           (unsigned long long)LONG_MAX + 1U,
                           &negative, &converted, &overflow);
    if (!converted) return 0;
    if (overflow) {
        errno = ERANGE;
        return negative ? LONG_MIN : LONG_MAX;
    }
    if (negative) {
        if (value > limit + 1U) {
            errno = ERANGE;
            return LONG_MIN;
        }
        if (value == limit + 1U) return LONG_MIN;
        return -(long)value;
    }
    if (value > limit) {
        errno = ERANGE;
        return LONG_MAX;
    }
    return (long)value;
}

unsigned long strtoul(const char *text, char **end, int base) {
    bool negative;
    bool converted;
    bool overflow;
    unsigned long value = (unsigned long)parse_unsigned(
        text, end, base, (unsigned long long)ULONG_MAX,
        &negative, &converted, &overflow);
    if (!converted) return 0U;
    if (overflow) {
        errno = ERANGE;
        return ULONG_MAX;
    }
    if (negative) value = (unsigned long)(0U - value);
    return value;
}

long long strtoll(const char *text, char **end, int base) {
    bool negative;
    bool converted;
    bool overflow;
    unsigned long long value = parse_unsigned(
        text, end, base, (unsigned long long)LLONG_MAX + 1U,
        &negative, &converted, &overflow);
    if (!converted) return 0;
    if (overflow) {
        errno = ERANGE;
        return negative ? LLONG_MIN : LLONG_MAX;
    }
    if (negative) {
        if (value > (unsigned long long)LLONG_MAX + 1U) {
            errno = ERANGE;
            return LLONG_MIN;
        }
        if (value == (unsigned long long)LLONG_MAX + 1U) return LLONG_MIN;
        return -(long long)value;
    }
    if (value > (unsigned long long)LLONG_MAX) {
        errno = ERANGE;
        return LLONG_MAX;
    }
    return (long long)value;
}

unsigned long long strtoull(const char *text, char **end, int base) {
    bool negative;
    bool converted;
    bool overflow;
    unsigned long long value = parse_unsigned(text, end, base, ULLONG_MAX,
                                              &negative, &converted,
                                              &overflow);
    if (!converted) return 0U;
    if (overflow) {
        errno = ERANGE;
        return ULLONG_MAX;
    }
    if (negative) value = 0U - value;
    return value;
}

intmax_t strtoimax(const char *text, char **end, int base) {
    return (intmax_t)strtoll(text, end, base);
}

uintmax_t strtoumax(const char *text, char **end, int base) {
    return (uintmax_t)strtoull(text, end, base);
}

int atoi(const char *text) {
    return (int)strtol(text, 0, 10);
}

long atol(const char *text) {
    return strtol(text, 0, 10);
}

long long atoll(const char *text) {
    return strtoll(text, 0, 10);
}

__attribute__((target("sse"))) double atof(const char *text) {
    return strtod(text, 0);
}

typedef struct parsed_float {
    long double value;
    const char *end;
    bool converted;
    bool nonzero;
    bool special;
} parsed_float_t;

static bool match_word(const char *text, const char *word) {
    while (*word != '\0') {
        if (tolower((unsigned char)*text++) != (unsigned char)*word++) return false;
    }
    return true;
}

static int bounded_add(int value, int amount) {
    if (amount > 0 && value > 20000 - amount) return 20000;
    if (amount < 0 && value < -20000 - amount) return -20000;
    return value + amount;
}

static int parse_float_exponent(const char **cursor, int marker) {
    const char *start = *cursor;
    const char *scan = start + 1;
    bool negative = false;
    int exponent = 0;
    if (tolower((unsigned char)*start) != marker) return 0;
    if (*scan == '+' || *scan == '-') {
        negative = *scan == '-';
        ++scan;
    }
    if (!isdigit((unsigned char)*scan)) return 0;
    do {
        if (exponent < 20000) {
            exponent = exponent * 10 + (*scan - '0');
            if (exponent > 20000) exponent = 20000;
        }
        ++scan;
    } while (isdigit((unsigned char)*scan));
    *cursor = scan;
    return negative ? -exponent : exponent;
}

static long double scale_float(long double value, unsigned int base,
                               int exponent) {
    long double factor = (long double)base;
    unsigned int power = (unsigned int)(exponent < 0 ? -exponent : exponent);
    while (power != 0U) {
        if ((power & 1U) != 0U) {
            value = exponent < 0 ? value / factor : value * factor;
        }
        power >>= 1;
        if (power != 0U) factor *= factor;
    }
    return value;
}

static parsed_float_t parse_float(const char *text) {
    parsed_float_t result = {0.0L, text, false, false, false};
    const char *cursor = skip_space(text);
    bool negative = false;
    if (*cursor == '+' || *cursor == '-') {
        negative = *cursor == '-';
        ++cursor;
    }
    if (match_word(cursor, "inf")) {
        cursor += 3;
        if (match_word(cursor, "inity")) cursor += 5;
        result.value = __builtin_huge_vall();
        result.end = cursor;
        result.converted = true;
        result.special = true;
    } else if (match_word(cursor, "nan")) {
        cursor += 3;
        if (*cursor == '(') {
            const char *payload = cursor + 1;
            const char *close = payload;
            while (isalnum((unsigned char)*close) || *close == '_') ++close;
            if (*close == ')') cursor = close + 1;
        }
        result.value = __builtin_nanl("");
        result.end = cursor;
        result.converted = true;
        result.special = true;
    } else {
        unsigned int base = 10U;
        unsigned int kept_limit = 19U;
        int exponent_scale = 1;
        int exponent_marker = 'e';
        unsigned int kept = 0U;
        unsigned int first_discarded = 0U;
        long double significand = 0.0L;
        int fractional_digits = 0;
        int discarded_digits = 0;
        bool after_point = false;
        bool digit_seen = false;
        bool discarded_nonzero = false;

        if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X') &&
            (isxdigit((unsigned char)cursor[2]) ||
             (cursor[2] == '.' && isxdigit((unsigned char)cursor[3])))) {
            base = 16U;
            kept_limit = 16U;
            exponent_scale = 4;
            exponent_marker = 'p';
            cursor += 2;
        }
        for (;;) {
            unsigned int digit;
            if (digit_value((unsigned char)*cursor, &digit) && digit < base) {
                digit_seen = true;
                if (after_point) fractional_digits = bounded_add(fractional_digits, 1);
                if (result.nonzero || digit != 0U) {
                    result.nonzero = true;
                    if (kept < kept_limit) {
                        significand = significand * (long double)base +
                                      (long double)digit;
                        ++kept;
                    } else {
                        if (discarded_digits == 0) first_discarded = digit;
                        else if (digit != 0U) discarded_nonzero = true;
                        discarded_digits = bounded_add(discarded_digits, 1);
                    }
                }
                ++cursor;
            } else if (!after_point && *cursor == '.') {
                after_point = true;
                ++cursor;
            } else {
                break;
            }
        }
        if (digit_seen) {
            int exponent = bounded_add(discarded_digits, -fractional_digits);
            int explicit_exponent = parse_float_exponent(&cursor, exponent_marker);
            if (discarded_digits != 0 &&
                (first_discarded > base / 2U ||
                 (first_discarded == base / 2U &&
                  (discarded_nonzero ||
                   (((unsigned long long)significand & 1U) != 0U))))) {
                significand += 1.0L;
            }
            result.value = scale_float(significand, base, exponent);
            if (exponent_scale != 1) {
                result.value = scale_float(result.value, 2U, explicit_exponent);
            } else {
                result.value = scale_float(result.value, base, explicit_exponent);
            }
            result.end = cursor;
            result.converted = true;
        }
    }
    if (negative) result.value = -result.value;
    return result;
}

static long double float_absolute(long double value) {
    return value < 0.0L ? -value : value;
}

__attribute__((target("sse"))) double strtod(const char *text, char **end) {
    if (text == 0) {
        if (end != 0) *end = 0;
        errno = EINVAL;
        return 0.0;
    }
    parsed_float_t parsed = parse_float(text);
    if (end != 0) *end = (char *)(parsed.converted ? parsed.end : text);
    if (!parsed.converted) return 0.0;
    long double magnitude = float_absolute(parsed.value);
    double value = (double)parsed.value;
    if (!parsed.special && parsed.nonzero &&
        (magnitude > (long double)__DBL_MAX__ || value == 0.0 ||
         float_absolute((long double)value) < (long double)__DBL_MIN__)) {
        errno = ERANGE;
    }
    return value;
}

__attribute__((target("sse"))) float strtof(const char *text, char **end) {
    if (text == 0) {
        if (end != 0) *end = 0;
        errno = EINVAL;
        return 0.0F;
    }
    parsed_float_t parsed = parse_float(text);
    if (end != 0) *end = (char *)(parsed.converted ? parsed.end : text);
    if (!parsed.converted) return 0.0F;
    long double magnitude = float_absolute(parsed.value);
    float value = (float)parsed.value;
    if (!parsed.special && parsed.nonzero &&
        (magnitude > (long double)__FLT_MAX__ || value == 0.0F ||
         float_absolute((long double)value) < (long double)__FLT_MIN__)) {
        errno = ERANGE;
    }
    return value;
}

long double strtold(const char *text, char **end) {
    if (text == 0) {
        if (end != 0) *end = 0;
        errno = EINVAL;
        return 0.0L;
    }
    parsed_float_t parsed = parse_float(text);
    if (end != 0) *end = (char *)(parsed.converted ? parsed.end : text);
    if (!parsed.converted) return 0.0L;
    long double magnitude = float_absolute(parsed.value);
    if (!parsed.special && parsed.nonzero &&
        (magnitude > __LDBL_MAX__ || parsed.value == 0.0L ||
         magnitude < __LDBL_MIN__)) {
        errno = ERANGE;
    }
    return parsed.value;
}

int abs(int value) {
    /* Avoid invoking signed-overflow UB for INT_MIN.  The ISO contract leaves
     * that input undefined; preserving it is the least surprising freestanding
     * behavior and avoids a divide/negate trap in optimized builds. */
    return value == INT_MIN ? INT_MIN : (value < 0 ? -value : value);
}

long labs(long value) {
    return value == LONG_MIN ? LONG_MIN : (value < 0 ? -value : value);
}

long long llabs(long long value) {
    return value == LLONG_MIN ? LLONG_MIN : (value < 0 ? -value : value);
}

div_t div(int numerator, int denominator) {
    div_t result = {0, 0};
    if (denominator == 0) {
        errno = EDOM;
        return result;
    }
    if (numerator == INT_MIN && denominator == -1) {
        result.quot = INT_MIN;
        return result;
    }
    result.quot = numerator / denominator;
    result.rem = numerator % denominator;
    return result;
}

ldiv_t ldiv(long numerator, long denominator) {
    ldiv_t result = {0, 0};
    if (denominator == 0) {
        errno = EDOM;
        return result;
    }
    if (numerator == LONG_MIN && denominator == -1) {
        result.quot = LONG_MIN;
        return result;
    }
    result.quot = numerator / denominator;
    result.rem = numerator % denominator;
    return result;
}

lldiv_t lldiv(long long numerator, long long denominator) {
    lldiv_t result = {0, 0};
    if (denominator == 0) {
        errno = EDOM;
        return result;
    }
    if (numerator == LLONG_MIN && denominator == -1) {
        result.quot = LLONG_MIN;
        return result;
    }
    result.quot = numerator / denominator;
    result.rem = numerator % denominator;
    return result;
}

static unsigned int g_random_state = 1U;

void srand(unsigned int seed) {
    g_random_state = seed == 0U ? 1U : seed;
}

int rand(void) {
    g_random_state = g_random_state * 1103515245U + 12345U;
    return (int)((g_random_state >> 1U) & RAND_MAX);
}

int rand_r(unsigned int *seed) {
    if (seed == 0) {
        errno = EINVAL;
        return -1;
    }
    *seed = *seed * 1103515245U + 12345U;
    return (int)((*seed >> 1U) & RAND_MAX);
}

int initstate_r(unsigned int seed, char *state_buffer, size_t state_length,
               struct random_data *buffer) {
    if (state_buffer == 0 || state_length < 8U || buffer == 0) {
        errno = EINVAL;
        return -1;
    }
    buffer->state = seed == 0U ? 1U : seed;
    return 0;
}

int random_r(struct random_data *buffer, int32_t *result) {
    if (buffer == 0 || result == 0) {
        errno = EINVAL;
        return -1;
    }
    buffer->state = buffer->state * 1103515245U + 12345U;
    *result = (int32_t)((buffer->state >> 1U) & RAND_MAX);
    return 0;
}

static void swap_bytes(unsigned char *left, unsigned char *right, size_t size) {
    /* 大目录条目按字宽交换；内建复制允许未对齐地址且不违反别名规则。 */
    while (size >= sizeof(uint64_t)) {
        uint64_t a, b;
        __builtin_memcpy(&a, left, sizeof(a));
        __builtin_memcpy(&b, right, sizeof(b));
        __builtin_memcpy(left, &b, sizeof(b));
        __builtin_memcpy(right, &a, sizeof(a));
        left += sizeof(a);
        right += sizeof(b);
        size -= sizeof(a);
    }
    while (size-- != 0U) {
        unsigned char value = *left;
        *left++ = *right;
        *right++ = value;
    }
}

static void sift_down(unsigned char *base, size_t root, size_t count,
                      size_t size,
                      int (*compare)(const void *, const void *)) {
    for (;;) {
        size_t child = root * 2U + 1U;
        size_t largest = root;
        if (child >= count) return;
        if (compare(base + largest * size, base + child * size) < 0) largest = child;
        if (child + 1U < count &&
            compare(base + largest * size, base + (child + 1U) * size) < 0) {
            largest = child + 1U;
        }
        if (largest == root) return;
        swap_bytes(base + root * size, base + largest * size, size);
        root = largest;
    }
}

void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *)) {
    unsigned char *bytes = (unsigned char *)base;
    if (bytes == 0 || count < 2U || size == 0U || compare == 0) return;
    for (size_t root = count / 2U; root != 0U; --root) {
        sift_down(bytes, root - 1U, count, size, compare);
    }
    for (size_t end = count - 1U; end != 0U; --end) {
        swap_bytes(bytes, bytes + end * size, size);
        sift_down(bytes, 0U, end, size, compare);
    }
}

void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *)) {
    const unsigned char *bytes = (const unsigned char *)base;
    size_t first = 0U;
    if (key == 0 || bytes == 0 || size == 0U || compare == 0) return 0;
    while (first < count) {
        size_t middle = first + (count - first) / 2U;
        int relation = compare(key, bytes + middle * size);
        if (relation == 0) return (void *)(bytes + middle * size);
        if (relation < 0) count = middle;
        else first = middle + 1U;
    }
    return 0;
}

int system(const char *command) {
    if (command == 0) return 0;
    errno = ENOSYS;
    return -1;
}

static bool has_six_template_markers(const char *template_name) {
    size_t length;
    if (template_name == 0) return false;
    length = strlen(template_name);
    return length >= 6U &&
           template_name[length - 1U] == 'X' &&
           template_name[length - 2U] == 'X' &&
           template_name[length - 3U] == 'X' &&
           template_name[length - 4U] == 'X' &&
           template_name[length - 5U] == 'X' &&
           template_name[length - 6U] == 'X';
}

static char base36_digit(unsigned int value) {
    return value < 10U ? (char)('0' + value) : (char)('a' + value - 10U);
}

int mkstemp(char *template_name) {
    size_t length;
    unsigned int attempt;
    if (!has_six_template_markers(template_name)) {
        errno = EINVAL;
        return -1;
    }
    length = strlen(template_name);
    for (attempt = 0U; attempt < 1000000U; ++attempt) {
        unsigned int value = attempt;
        for (size_t index = 0U; index < 6U; ++index) {
            template_name[length - 1U - index] = base36_digit(value % 36U);
            value /= 36U;
        }
        int descriptor = open(template_name,
                              O_RDWR | O_CREAT | O_EXCL, 0600);
        if (descriptor >= 0) return descriptor;
        if (errno != EEXIST) return -1;
    }
    template_name[0] = '\0';
    errno = EEXIST;
    return -1;
}

char *mkdtemp(char *template_name) {
    size_t length;
    if (!has_six_template_markers(template_name)) {
        errno = EINVAL;
        return 0;
    }
    length = strlen(template_name);
    for (unsigned int attempt = 0U; attempt < 1000000U; ++attempt) {
        unsigned int value = attempt;
        for (size_t index = 0U; index < 6U; ++index) {
            template_name[length - 1U - index] = base36_digit(value % 36U);
            value /= 36U;
        }
        if (mkdir(template_name, 0700U) == 0) return template_name;
        if (errno != EEXIST) return 0;
    }
    template_name[0] = '\0';
    errno = EEXIST;
    return 0;
}

char *mktemp(char *template_name) {
    size_t length;
    if (!has_six_template_markers(template_name)) {
        errno = EINVAL;
        return template_name;
    }
    length = strlen(template_name);
    for (unsigned int attempt = 0U; attempt < 1000000U; ++attempt) {
        unsigned int value = attempt;
        for (size_t index = 0U; index < 6U; ++index) {
            template_name[length - 1U - index] = base36_digit(value % 36U);
            value /= 36U;
        }
        if (access(template_name, F_OK) < 0 && errno == ENOENT) return template_name;
    }
    template_name[0] = '\0';
    errno = EEXIST;
    return template_name;
}

int getsubopt(char **optionp, char *const *tokens, char **valuep) {
    char *option;
    char *end;
    char *separator;
    size_t option_length;
    int result = -1;

    if (optionp == 0 || *optionp == 0 || tokens == 0 || valuep == 0) {
        errno = EINVAL;
        return -1;
    }
    option = *optionp;
    end = strchr(option, ',');
    if (end != 0) {
        *end = '\0';
        *optionp = end + 1U;
    } else {
        *optionp = option + strlen(option);
    }
    separator = strchr(option, '=');
    if (separator != 0) {
        *separator = '\0';
        *valuep = separator + 1U;
    } else {
        *valuep = 0;
    }
    option_length = strlen(option);
    for (int index = 0; tokens[index] != 0; ++index) {
        if (strlen(tokens[index]) == option_length &&
            strcmp(tokens[index], option) == 0) {
            result = index;
            break;
        }
    }
    return result;
}
