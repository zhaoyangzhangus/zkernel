#pragma once
#ifndef OS_KERNEL_PERF_H
#define OS_KERNEL_PERF_H

#include "base.h"

#define KERNEL_PERF_SAMPLE_COUNT 128U

typedef struct kernel_perf_report {
    uint64_t samples;
    uint64_t min_tsc;
    uint64_t median_tsc;
    uint64_t p95_tsc;
    uint64_t average_tsc;
    uint64_t max_tsc;
    uint64_t fastpath_hits;
    uint64_t fastpath_refills;
} kernel_perf_report_t;

bool kernel_perf_benchmark(kernel_perf_report_t *report);
/* 下方宏使用的带位置信息边界。保留调用位置的文件名和行号，可像调试阶段一样直接定位指标来源。 */
void kernel_perf_emit_scope_at(const char *name, uint64_t start_tsc,
                              const char *file, uint32_t line);
void kernel_perf_emit_scope(const char *name, uint64_t start_tsc);
void kernel_perf_emit_value_at(const char *name, uint64_t value,
                              const char *file, uint32_t line);
void kernel_perf_emit_value(const char *name, uint64_t value);
/* 由 perf 所有者输出固定格式的 kmalloc 报告，并保留调用方源码位置，供调试器和基准测试诊断使用。 */
void kernel_perf_emit_report_at(const kernel_perf_report_t *report,
                               const char *file, uint32_t line);
void kernel_perf_emit_report(const kernel_perf_report_t *report);

#define kernel_perf_emit_scope(name, start_tsc) \
    kernel_perf_emit_scope_at((name), (start_tsc), __FILE__, __LINE__)

#define kernel_perf_emit_value(name, value) \
    kernel_perf_emit_value_at((name), (value), __FILE__, __LINE__)

#define kernel_perf_emit_report(report) \
    kernel_perf_emit_report_at((report), __FILE__, __LINE__)

#endif
