#pragma once
#include "vfs.h"

typedef struct file_lock_range {
    uint64_t owner;
    int64_t first, last; /* inclusive; INT64_MAX includes future EOF */
    int16_t type;       /* Linux RDLCK/WRLCK/UNLCK */
} file_lock_range_t;

kstatus_t file_lock_query(vnode_t *node, const file_lock_range_t *request,
                          file_lock_range_t *conflict);
kstatus_t file_lock_set(vnode_t *node, const file_lock_range_t *request);
/* 调用方负责将逻辑 fd 关闭与加锁操作串行化。释放 fd 表锁后再通知等待者。临时 fd 固定不等于关闭。 */
void file_lock_release_file(vnode_t *node, uint64_t owner);
void file_lock_release_owner(uint64_t owner);
kstatus_t file_lock_begin_wait(vnode_t *node, const file_lock_range_t *request,
                               uint32_t *ticket);
void file_lock_end_wait(uint32_t ticket);
