#pragma once
#pragma once
#include "abi.h"

enum os_vm_prot {
    OS_VM_READ  = 1u << 0,
    OS_VM_WRITE = 1u << 1,
    OS_VM_EXEC  = 1u << 2,
};

enum os_vm_flags {
    OS_VM_PRIVATE = 1u << 0,
    OS_VM_SHARED  = 1u << 1,
    OS_VM_FIXED   = 1u << 2,
    OS_VM_STACK   = 1u << 3,
};

/* VM_SYNC 标志遵循通用 msync 约定。当前 ASYNC 操作也会同步完成，
 * 保证强于最低要求。 */
enum os_vm_sync_flags {
    OS_VM_SYNC_ASYNC      = 1u << 0,
    OS_VM_SYNC_INVALIDATE = 1u << 1,
    OS_VM_SYNC_SYNC       = 1u << 2,
};

/* VM_ADVISE 的取值刻意沿用 POSIX 数值空间。 */
enum os_vm_advice {
    OS_VM_ADVICE_NORMAL    = 0,
    OS_VM_ADVICE_RANDOM    = 1,
    OS_VM_ADVICE_SEQUENTIAL = 2,
    OS_VM_ADVICE_WILLNEED  = 3,
    OS_VM_ADVICE_DONTNEED  = 4,
    OS_VM_ADVICE_DONTFORK  = 10,
    OS_VM_ADVICE_DOFORK    = 11,
    OS_VM_ADVICE_MERGEABLE = 12,
    OS_VM_ADVICE_UNMERGEABLE = 13,
    OS_VM_ADVICE_HUGEPAGE  = 14,
    OS_VM_ADVICE_NOHUGEPAGE = 15,
    OS_VM_ADVICE_DONTDUMP  = 16,
    OS_VM_ADVICE_DODUMP    = 17,
};

typedef struct os_vm_map_args {
    os_versioned_header_t hdr;
    uint64_t address;
    uint64_t length;
    uint64_t offset;
    os_handle_t object;
    uint32_t prot;
    uint32_t flags;
} os_vm_map_args_t;

/* VM_SHARE 创建一个可由多个地址空间映射的匿名共享段。 */
typedef struct os_vm_share_args {
    os_versioned_header_t hdr;
    uint64_t size;
    uint32_t flags;
    uint32_t reserved;
    os_handle_t section;
} os_vm_share_args_t;
