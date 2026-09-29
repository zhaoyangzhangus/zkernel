#ifndef __KERNEL_MM_PAGE_FAULT_H__
#define __KERNEL_MM_PAGE_FAULT_H__

#include <stdbool.h>
#include <stdint.h>

#include "vm.h"

/* 当前阶段只有一个 kernel address space。 */
void page_fault_bind_space(vm_space_t *space);

/*
 * 处理 x86 #PF。返回 true 表示 fault 已修复，可以 iret 后重试指令。
 * 第一版只处理 VM_ATTR_LAZY 的 4K demand-zero page。
 */
bool page_fault_handle(uint64_t address, uint64_t error_code);

#endif
