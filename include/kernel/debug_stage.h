#pragma once

#include <kernel/base.h>

/*
 * 启动/运行阶段的定义刻意保持精简且稳定。数值属于调试日志约定的一部分：
 * 即使内核尚处于无法输出符号名称的早期启动阶段，也能解码串口跟踪记录。
 */
typedef enum debug_phase {
    OS_DEBUG_PHASE_BOOT = 1,
    OS_DEBUG_PHASE_CPU = 2,
    OS_DEBUG_PHASE_MEMORY = 3,
    OS_DEBUG_PHASE_SCHEDULER = 4,
    OS_DEBUG_PHASE_DRIVER = 5,
    OS_DEBUG_PHASE_STORAGE = 6,
    OS_DEBUG_PHASE_NETWORK = 7,
    OS_DEBUG_PHASE_DISPLAY = 8,
    OS_DEBUG_PHASE_USER = 9,
    OS_DEBUG_PHASE_USER_RUNTIME = 10,
    OS_DEBUG_PHASE_DESKTOP = 11,

    /*
    * 实现规范使用独立的 0..20 阶段约定。为其分配单独的数值范围，
    * 使现有运行阶段名称和旧日志解析器保持稳定，同时允许调试镜像
    * 定位每个规范阶段。
     */
    OS_DEBUG_PHASE_SPEC_0 = 32,
    OS_DEBUG_PHASE_SPEC_1 = 33,
    OS_DEBUG_PHASE_SPEC_2 = 34,
    OS_DEBUG_PHASE_SPEC_3 = 35,
    OS_DEBUG_PHASE_SPEC_4 = 36,
    OS_DEBUG_PHASE_SPEC_5 = 37,
    OS_DEBUG_PHASE_SPEC_6 = 38,
    OS_DEBUG_PHASE_SPEC_7 = 39,
    OS_DEBUG_PHASE_SPEC_8 = 40,
    OS_DEBUG_PHASE_SPEC_9 = 41,
    OS_DEBUG_PHASE_SPEC_10 = 42,
    OS_DEBUG_PHASE_SPEC_11 = 43,
    OS_DEBUG_PHASE_SPEC_12 = 44,
    OS_DEBUG_PHASE_SPEC_13 = 45,
    OS_DEBUG_PHASE_SPEC_14 = 46,
    OS_DEBUG_PHASE_SPEC_15 = 47,
    OS_DEBUG_PHASE_SPEC_16 = 48,
    OS_DEBUG_PHASE_SPEC_17 = 49,
    OS_DEBUG_PHASE_SPEC_18 = 50,
    OS_DEBUG_PHASE_SPEC_19 = 51,
    OS_DEBUG_PHASE_SPEC_20 = 52,

    /* 重构路线图阶段 0..9 使用第三个数值范围。这些记录描述仓库/构建门禁，
     * 并不代表操作系统实现阶段。 */
    OS_DEBUG_PHASE_REFACTOR_0 = 64,
    OS_DEBUG_PHASE_REFACTOR_1 = 65,
    OS_DEBUG_PHASE_REFACTOR_2 = 66,
    OS_DEBUG_PHASE_REFACTOR_3 = 67,
    OS_DEBUG_PHASE_REFACTOR_4 = 68,
    OS_DEBUG_PHASE_REFACTOR_5 = 69,
    OS_DEBUG_PHASE_REFACTOR_6 = 70,
    OS_DEBUG_PHASE_REFACTOR_7A = 71,
    OS_DEBUG_PHASE_REFACTOR_7B = 72,
    OS_DEBUG_PHASE_REFACTOR_8 = 73,
    OS_DEBUG_PHASE_REFACTOR_9 = 74,
} debug_phase_t;

/* 当前启动和运行路径共用的步骤标识。 */
enum {
    OS_DEBUG_STEP_ENTER = 1,
    OS_DEBUG_STEP_LOAD = 2,
    OS_DEBUG_STEP_CREATE = 3,
    OS_DEBUG_STEP_START = 4,
    OS_DEBUG_STEP_READY = 5,
    OS_DEBUG_STEP_EXIT = 6,
    OS_DEBUG_STEP_WAIT_ONE_ENTER = 7,
    OS_DEBUG_STEP_WAIT_ONE_BLOCK = 8,
    OS_DEBUG_STEP_WAIT_ONE_RETURN = 9,
    OS_DEBUG_STEP_THREAD_EXIT_ENTER = 10,
    OS_DEBUG_STEP_THREAD_EXIT_PUBLISHED = 11,
    OS_DEBUG_STEP_THREAD_EXIT_SCHEDULE = 12,
    OS_DEBUG_STEP_USER_MARK = 13,
    OS_DEBUG_STEP_STRESS = 14,
    /* 此构建包含该阶段，但暂不宣称其验收实现已经完成。 */
    OS_DEBUG_STEP_PENDING = 15,
    /* 重构边界已落地，但完整的验收门禁仍未通过。 */
    OS_DEBUG_STEP_PROGRESS = 16,
    /* 致命路径中止了当前阶段。仅在启动失败诊断中输出状态和调用位置。 */
    OS_DEBUG_STEP_FAIL = 17,
};

/*
 * 发布阶段转换。所有构建中，调试器都可以读取最新的阶段、步骤、值和序号；
 * OS_DEBUG_SERIAL 还会额外输出一条仅发送至 COM1 的记录。此功能不会写入 GOP 帧缓冲。
 *
 * 调用方使用下方宏，以便每条记录都携带其来源位置。显式的 *_at 入口与宏分开，
 * 这样实现仍可为调试器和兼容调用方导出普通函数符号。
 */
void kernel_debug_stage_at(uint16_t phase, uint16_t step, uint32_t value,
                           const char *file, uint32_t line);
void kernel_debug_stage_fail_at(uint16_t phase, uint16_t step, int64_t status,
                                const char *file, uint32_t line);

/* 为调试器表达式和较早的树外调用方保留稳定符号。 */
void kernel_debug_stage(uint16_t phase, uint16_t step, uint32_t value);
void kernel_debug_stage_fail(uint16_t phase, uint16_t step, int64_t status);

#define kernel_debug_stage(phase, step, value) \
    kernel_debug_stage_at((phase), (step), (value), __FILE__, __LINE__)
#define kernel_debug_stage_fail(phase, step, status) \
    kernel_debug_stage_fail_at((phase), (step), (status), __FILE__, __LINE__)

/* 让常见阶段转换记录真实调用方的源代码位置。 */
#define kernel_debug_stage_enter(phase) \
    kernel_debug_stage_at((phase), OS_DEBUG_STEP_ENTER, 0U, \
                          __FILE__, __LINE__)
#define kernel_debug_stage_ready(phase) \
    kernel_debug_stage_at((phase), OS_DEBUG_STEP_READY, 1U, \
                          __FILE__, __LINE__)
#define kernel_debug_stage_pending(phase) \
    kernel_debug_stage_at((phase), OS_DEBUG_STEP_PENDING, 0U, \
                          __FILE__, __LINE__)

/* 运行时自测会在其用户映像执行期间启用此精简事件流。 */
void debug_trace_set(bool enabled);
bool debug_trace_enabled(void);
void debug_trace_stage_at(uint16_t phase, uint16_t step, uint32_t value,
                                 const char *file, uint32_t line);
void debug_trace_stage(uint16_t phase, uint16_t step, uint32_t value);
#define debug_trace_stage(phase, step, value) \
    debug_trace_stage_at((phase), (step), (value), __FILE__, __LINE__)

uint64_t kernel_debug_stage_sequence(void);
uint16_t kernel_debug_stage_phase(void);
uint16_t kernel_debug_stage_step(void);
uint32_t kernel_debug_stage_value(void);
int64_t kernel_debug_stage_status(void);
const char *kernel_debug_stage_file(void);
uint32_t kernel_debug_stage_line(void);
