#pragma once

#include <kernel/base.h>

struct OS_FAT32;
struct os_boot_info;

/* 真机测试构建会在根 FAT 卷保存持久启动跟踪，并在测试通过或发生致命故障后返回宿主系统。 */
bool realtest_enabled(void);
void realtest_boot_info(const struct os_boot_info *info);
void realtest_mark(const char *state);
void realtest_checkpoint(const char *state);
/* 驱动/RM 检查点可能在中断开启或持有供应商锁时运行。它们不得访问 UEFI 变量或 FAT/VFS；
 * 累积的记录会在下一个安全运行边界刷新。 */
void realtest_checkpoint_deferred(const char *state);
void realtest_mark_number(const char *prefix, uint32_t value);
void realtest_clear_failure_state(void);
void realtest_mark_value(const char *prefix, uint32_t value);
void realtest_mark_xhci_control(const char *kind, uint32_t error,
                                       uint32_t slot, uint32_t port,
                                       uint32_t length, uint32_t event_index,
                                       uint32_t event_cycle);
void realtest_disable_runtime_services(void);
void realtest_capture(const char *text);
void realtest_fat_ready(struct OS_FAT32 *filesystem);
void realtest_fat_lost(const char *reason);
void realtest_filesystem_ready(void);
bool realtest_flush(void);
void realtest_record_failure(const char *file, uint32_t line);
void realtest_record_exception(uint64_t vector, uint64_t error_code,
                                      uint64_t rip, uint64_t fault_address,
                                      bool has_fault_address);
/* 即使驱动线程卡在阻塞式供应商调用中，真机测试路径也必须能退出。
 * 这些操作可从时钟中断无锁执行；超时重启只写串口，不访问 FAT 捕获状态。 */
void realtest_timeout_start(void);
void realtest_timeout_stop(void);
bool realtest_timeout_poll(uint64_t now_tsc);
__attribute__((noreturn)) void realtest_timeout_reboot(void);
void realtest_finish_success(void);
void realtest_finish_failure(void) __attribute__((noreturn));
