#pragma once

#include <arch/x86_64/context.h>
#include <kernel/base.h>

/* 用户地址检查只是第一道边界，真正的访存必须经过带异常修复表的复制函数。 */
bool x86_user_range_valid(const void __user *pointer, size_t size);
kstatus_t copy_from_user(void *destination, const void __user *source, size_t size);
/* capacity 包含终止符；缺少终止符返回 K_ENAMETOOLONG，失败时目标可能已部分写入。 */
kstatus_t copy_string_from_user(char *destination, const char __user *source,
                                 size_t capacity);
kstatus_t copy_to_user(void __user *destination, const void *source, size_t size);
kstatus_t get_user_u32(uint32_t *value, const uint32_t __user *source);
kstatus_t put_user_u32(uint32_t __user *destination, uint32_t value);

void x86_uaccess_init(bool smap_enabled);
bool x86_uaccess_fault_site(const arch_trap_frame_t *frame);
bool x86_uaccess_fixup(arch_trap_frame_t *frame);
