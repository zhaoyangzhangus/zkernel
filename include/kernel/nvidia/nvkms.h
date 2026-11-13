#pragma once

#include <stdbool.h>

#include <kernel/base.h>

/*
 * 官方 595.84 NVKMS 核心面向 System 的生命周期接口。返回值特意设为不透明类型，避免普通内核代码
 * 依赖厂商 KAPI 类型布局。
 */
kstatus_t nv_nvkms_init(void);
void nv_nvkms_shutdown(void);
bool nv_nvkms_ready(void);
const void *nv_nvkms_functions(void);

/* 启动一个真实的 NVKMS 显示头，并将其注册到 System 显示核心。 */
kstatus_t nv_nvkms_display_init(void);
bool nv_nvkms_display_active(void);

/*
 * 从真实可调度的 Ring0 线程中加载 NVKMS 并执行首次原生模式设置。调度器启动后的 BSP 续执行路径
 * 基于逻辑空闲入口运行，因此不得持有可休眠的 System 互斥锁。
 */
kstatus_t nv_nvkms_bootstrap_init(void);
