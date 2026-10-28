#pragma once
#pragma once

#include "base.h"
#include "gpu.h"


/*
 * 原生扫描输出与翻页约定。
 *
 * display_commit_submit() 是公开的提交接口。未注册原生后端时，显示核心使用
 * GOP 复制回退路径；注册后，则在获取栅栏完成时将同一提交交给 atomic_commit。
 *
 * atomic_commit() 返回 K_OK 时，请求的异步所有权转交给后端。后端必须且只能
 * 调用一次 complete。
 *
 * complete(K_OK) 表示新表面已锁存用于扫描输出，显示引擎不再读取此前的扫描
 * 输出表面。通常这一时刻对应翻页完成或垂直消隐。显示核心仅在此回调之后触发
 * 用户的释放栅栏。
 *
 * 完成回调可以由 atomic_commit() 同步调用，也可以稍后在普通或延迟内核上下文
 * 中调用，但绝不能直接在硬中断上下文中执行。GPU 中断处理程序应先确认中断，
 * 再调度自身的下半部，之后才能调用 complete。
 */
typedef struct display_scanout_request {
    uint32_t output;
    gpu_allocation_t *buffer;
    uint64_t offset;
    uint64_t stride;
    uint32_t width;
    uint32_t height;
    uint32_t format;
} display_scanout_request_t;

typedef void (*display_scanout_complete_fn)(
    void *completion_context,
    kstatus_t status);

typedef struct display_scanout_backend {
    void *context;

    /* 可选的、有界线程上下文进度/超时检查；不得在中断上下文中调用。 */
    void (*poll)(void *context);

    kstatus_t (*atomic_commit)(
        void *context,
        const display_scanout_request_t *request,
        display_scanout_complete_fn complete,
        void *completion_context);
} display_scanout_backend_t;

/* 显示核心当前提供一个 GOP 输出；后续真实 GPU 后端复用同一提交接口。 */
bool display_core_init(uint64_t framebuffer_virtual,
                       uint64_t framebuffer_size,
                       uint32_t width,
                       uint32_t height,
                       uint32_t pixels_per_scanline,
                       uint32_t framebuffer_format,
                       const uint32_t masks[4]);
bool display_core_is_initialized(void);
uint64_t display_core_framebuffer_virtual(void);
bool display_core_query(uint32_t output, uint32_t *width, uint32_t *height,
                        uint32_t *stride, uint32_t *format);

/*
 * 为指定输出注册原子原生扫描输出实现。
 *
 * P7 阶段有意将注册限制为一次。后端上下文及其函数在显示设备的整个生命周期内
 * 必须保持有效。
 */
kstatus_t display_core_register_scanout_backend(
    uint32_t output,
    const display_scanout_backend_t *backend);

bool display_core_has_native_scanout(uint32_t output);
void display_core_poll_scanout(void);

/* 通过已注册的原生后端提交一个完整帧的 CPU-WB 缓冲区，并等待其释放栅栏。
 * 这是启动时的交接测试，用于将原生翻页/事件传递与后续合成器接入分开验证。 */
bool display_core_native_self_test(void);

/*
 * 内核合成器快速路径。
 *
 * 源数据是普通 WB 内存中的 XRGB8888 格式；目标地址指向当前扫描输出映射。
 * 此函数不执行栅栏：调用方应批量处理多个跨度，并在事务或分块边界发出 SFENCE。
 */
void display_core_publish_xrgb8888_pixel(volatile uint32_t *destination, uint32_t pixel);
void display_core_publish_xrgb8888_span(volatile uint32_t *destination,
                                        const uint32_t *source,
                                        uint32_t pixels);

kstatus_t display_commit_submit(uint32_t output,
                                gpu_allocation_t *buffer,
                                uint64_t offset,
                                uint64_t stride,
                                uint32_t width,
                                uint32_t height,
                                uint32_t format,
                                gpu_fence_t *wait_fence,
                                uint64_t wait_value,
                                gpu_fence_t *signal_fence,
                                uint64_t signal_value);

bool display_core_self_test(void);
