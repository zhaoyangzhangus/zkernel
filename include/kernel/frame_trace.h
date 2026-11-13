#pragma once
#ifndef OS_KERNEL_FRAME_TRACE_H
#define OS_KERNEL_FRAME_TRACE_H

#include <kernel/perf.h>

#ifndef OS_ENABLE_FRAME_TRACE
#define OS_ENABLE_FRAME_TRACE 0
#endif

#if OS_ENABLE_FRAME_TRACE
#define kernel_frame_trace_emit_scope(name, start_tsc) \
    kernel_perf_emit_scope((name), (start_tsc))
#define kernel_frame_trace_emit_value(name, value) \
    kernel_perf_emit_value((name), (value))
#else
/* 正常热路径上不输出诊断，也不计算诊断参数。 */
#define kernel_frame_trace_emit_scope(name, start_tsc) ((void)0)
#define kernel_frame_trace_emit_value(name, value) ((void)0)
#endif

#endif
