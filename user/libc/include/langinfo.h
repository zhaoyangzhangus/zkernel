#pragma once

#include <locale.h>

typedef int nl_item;

/* libc++ Linux 区域设置适配器使用的 C/POSIX 区域项。 */
#define CODESET    14
#define D_T_FMT    0
#define D_FMT      1
#define T_FMT      2
#define T_FMT_AMPM 3
#define AM_STR      4
#define PM_STR      5
#define DAY_1       6
#define DAY_2       7
#define DAY_3       8
#define DAY_4       9
#define DAY_5      10
#define DAY_6      11
#define DAY_7      12
#define ABDAY_1    13
#define ABDAY_2    15
#define ABDAY_3    16
#define ABDAY_4    17
#define ABDAY_5    18
#define ABDAY_6    19
#define ABDAY_7    20
#define MON_1      21
#define MON_2      22
#define MON_3      23
#define MON_4      24
#define MON_5      25
#define MON_6      26
#define MON_7      27
#define MON_8      28
#define MON_9      29
#define MON_10     30
#define MON_11     31
#define MON_12     32
#define ABMON_1    33
#define ABMON_2    34
#define ABMON_3    35
#define ABMON_4    36
#define ABMON_5    37
#define ABMON_6    38
#define ABMON_7    39
#define ABMON_8    40
#define ABMON_9    41
#define ABMON_10   42
#define ABMON_11   43
#define ABMON_12   44
#define RADIXCHAR  45
#define THOUSEP    46
#define YESEXPR    54
#define NOEXPR     55

#ifdef __cplusplus
extern "C" {
#endif
char *nl_langinfo(nl_item item);
char *nl_langinfo_l(nl_item item, locale_t locale);
#ifdef __cplusplus
}
#endif
