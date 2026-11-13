#pragma once

#include <kernel/base.h>

/* 原生 Realtek RTL8126A 有线网卡后端。 */
bool rtl8126_hardware_present(void);
bool rtl8126_self_test(void);
bool rtl8126_interrupt_ready(void);
bool rtl8126_firmware_required(void);
void rtl8126_emit_diagnostic(void);
