#include <kernel/kmem.h>
#include <kernel/console.h>
#include <kernel/perf.h>
#include <kernel/telemetry.h>

/* 公共头文件会将普通调用映射到带位置信息的入口。 */
#undef kernel_perf_emit_scope
#undef kernel_perf_emit_value
#undef kernel_perf_emit_report

static void kernel_perf_sort(uint64_t *samples, uint32_t count) {
    for (uint32_t i = 1U; i < count; ++i) {
        uint64_t value = samples[i];
        uint32_t position = i;
        while (position != 0U && samples[position - 1U] > value) {
            samples[position] = samples[position - 1U];
            --position;
        }
        samples[position] = value;
    }
}

bool kernel_perf_benchmark(kernel_perf_report_t *report) {
    uint64_t samples[KERNEL_PERF_SAMPLE_COUNT];
    uint64_t minimum = UINT64_MAX;
    uint64_t maximum = 0U;
    uint64_t total = 0U;
    uint64_t hits_before;
    uint64_t hits_after;
    uint64_t refills_before;
    uint64_t refills_after;

    if (report == 0) return false;
    /* 预热后每次只做一个固定大小的小对象分配，保持 magazine 热路径稳定。 */
    for (uint32_t i = 0; i < 32U; ++i) {
        void *memory = kmalloc(64U, 0);
        if (memory == 0) return false;
        kfree(memory);
    }
    hits_before = kmem_fastpath_hits();
    refills_before = kmem_fastpath_refills();
    for (uint32_t i = 0; i < KERNEL_PERF_SAMPLE_COUNT; ++i) {
        uint64_t start = telemetry_timestamp();
        void *memory = kmalloc(64U, 0);
        uint64_t elapsed;
        if (memory == 0) return false;
        elapsed = telemetry_timestamp() - start;
        kfree(memory);
        if (elapsed < minimum) minimum = elapsed;
        if (elapsed > maximum) maximum = elapsed;
        samples[i] = elapsed;
        total += elapsed;
    }
    hits_after = kmem_fastpath_hits();
    refills_after = kmem_fastpath_refills();
    report->samples = KERNEL_PERF_SAMPLE_COUNT;
    report->min_tsc = minimum;
    kernel_perf_sort(samples, KERNEL_PERF_SAMPLE_COUNT);
    report->median_tsc = samples[KERNEL_PERF_SAMPLE_COUNT / 2U];
    report->p95_tsc = samples[((KERNEL_PERF_SAMPLE_COUNT * 95U) + 99U) /
                              100U - 1U];
    report->average_tsc = total / KERNEL_PERF_SAMPLE_COUNT;
    report->max_tsc = maximum;
    report->fastpath_hits = hits_after - hits_before;
    report->fastpath_refills = refills_after - refills_before;
    return report->fastpath_hits != 0U && report->min_tsc <= report->max_tsc;
}

void kernel_perf_emit_scope_at(const char *name, uint64_t start_tsc,
                              const char *file, uint32_t line) {
    uint64_t end_tsc;
    if (name == 0) return;
    end_tsc = telemetry_timestamp();
    kernel_serial_printf_serial_only(
        "OS_BENCH name=%s cycles=%llu loc=%s:%u\r\n",
        name, (unsigned long long)(end_tsc - start_tsc),
        file != 0 ? file : "<unknown>", line);
}

void kernel_perf_emit_scope(const char *name, uint64_t start_tsc) {
    kernel_perf_emit_scope_at(name, start_tsc, "<unknown>", 0U);
}

void kernel_perf_emit_value_at(const char *name, uint64_t value,
                              const char *file, uint32_t line) {
    if (name == 0) return;
    kernel_serial_printf_serial_only(
        "OS_BENCH_VALUE name=%s value=%llu loc=%s:%u\r\n",
        name, (unsigned long long)value,
        file != 0 ? file : "<unknown>", line);
}

void kernel_perf_emit_value(const char *name, uint64_t value) {
    kernel_perf_emit_value_at(name, value, "<unknown>", 0U);
}

void kernel_perf_emit_report_at(const kernel_perf_report_t *report,
                               const char *file, uint32_t line) {
    if (report == 0) return;
    kernel_serial_printf_serial_only(
        "OS_PERF_KMALLOC_OK SAMPLES=%llu MIN=%llu MEDIAN=%llu "
        "P95=%llu AVG=%llu MAX=%llu HITS=%llu REFILLS=%llu "
        "loc=%s:%u\r\n",
        (unsigned long long)report->samples,
        (unsigned long long)report->min_tsc,
        (unsigned long long)report->median_tsc,
        (unsigned long long)report->p95_tsc,
        (unsigned long long)report->average_tsc,
        (unsigned long long)report->max_tsc,
        (unsigned long long)report->fastpath_hits,
        (unsigned long long)report->fastpath_refills,
        file != 0 ? file : "<unknown>", line);
}

void kernel_perf_emit_report(const kernel_perf_report_t *report) {
    kernel_perf_emit_report_at(report, "<unknown>", 0U);
}
