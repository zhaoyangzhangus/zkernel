#pragma once
#include "abi.h"

typedef struct os_thread_create {
    os_versioned_header_t hdr;
    uint64_t entry;
    uint64_t stack_top;
    uint64_t fs_base;
    uint64_t argument;
    uint32_t flags;
    uint32_t reserved;
} os_thread_create_t;

/* 可选的第四个 THREAD_CREATE 参数。分配私有用户栈的调用方可请求内核在线程
 * 切离该栈后回收它。此描述符不放入 os_thread_create_t，以保留旧调用方使用的
 * 原始 ABI 布局。 */
typedef struct os_thread_stack {
    os_versioned_header_t hdr;
    uint64_t base;
    uint64_t size;
    uint32_t flags;
    uint32_t reserved;
} os_thread_stack_t;

#define OS_THREAD_STACK_OWNED (1U << 0)

enum os_thread_context_operation {
    OS_THREAD_CONTEXT_GET_FS = 0,
    OS_THREAD_CONTEXT_SET_FS = 1,
};

typedef struct os_process_info {
    os_versioned_header_t hdr;
    uint64_t pid;
    uint64_t parent_pid;
    uint32_t state;
    uint32_t reserved;
    int64_t exit_code;
} os_process_info_t;

#define OS_PROCESS_NAME_MAX 32U
#define OS_THREAD_NAME_MAX  32U

/* 供监视器使用的增量 ABI：现有枚举结构布局保持不变。 */
#define OS_TASK_PROTECTED (1U << 0)
#define OS_TASK_KERNEL    (1U << 1)
/* 只读聚合项，不是进程，也不是有效的终止目标。 */
#define OS_KERNEL_WORKERS_PID UINT64_MAX
typedef struct os_task_info {
    os_versioned_header_t hdr;
    uint64_t pid;
    uint64_t tid; /* 零表示查询整个进程。 */
    uint64_t create_time_ns;
    uint64_t runtime_ns;
    uint64_t rss_bytes;
    uint64_t committed_bytes;
    uint32_t cpu_count;
    uint32_t flags;
    uint32_t state;
    uint32_t thread_count;
} os_task_info_t;

typedef struct os_process_terminate {
    os_versioned_header_t hdr;
    uint64_t pid;
    uint64_t create_time_ns; /* 必须匹配已确认的快照，不能使用过期条目。 */
} os_process_terminate_t;

typedef struct os_thread_terminate {
    os_versioned_header_t hdr;
    uint64_t pid;
    uint64_t tid;
    uint64_t create_time_ns;
} os_thread_terminate_t;

/* 系统监视器使用的稳定只读进程快照。 */
typedef struct os_process_snapshot {
    os_versioned_header_t hdr;
    uint64_t pid;
    uint64_t parent_pid;
    uint32_t state;
    uint32_t thread_count;
    int64_t exit_code;
    uint64_t create_time_ns;
    char name[OS_PROCESS_NAME_MAX];
} os_process_snapshot_t;

typedef struct os_process_enumerate {
    os_versioned_header_t hdr;
    uint32_t index;
    uint32_t reserved;
    os_process_snapshot_t info;
} os_process_enumerate_t;

typedef struct os_thread_snapshot {
    os_versioned_header_t hdr;
    uint64_t tid;
    uint64_t process_pid;
    uint32_t state;
    uint32_t current_cpu;
    char name[OS_THREAD_NAME_MAX];
} os_thread_snapshot_t;

typedef struct os_thread_enumerate {
    os_versioned_header_t hdr;
    uint32_t index;
    uint32_t reserved;
    os_thread_snapshot_t info;
} os_thread_enumerate_t;

/* PROCESS_EXEC 通过新映像的辅助向量传递当前 libc 描述符表。内核句柄仍是所有权
 * 边界；这些记录仅保留用户态 fd 映射及其本地元数据。 */
#define OS_EXEC_FD_LIMIT      256U
#define OS_EXEC_FD_PATH_MAX   256U
#define OS_EXEC_FD_SOCKET     (1U << 0)
#define OS_EXEC_FD_PIPE       (1U << 1)
#define OS_EXEC_FD_PIPE_READ  (1U << 2)
#define OS_EXEC_FD_DEBUG      (1U << 3)
#define OS_AUX_FD_MAP  0x6F534446U

typedef struct os_exec_fd_entry {
    uint32_t descriptor;
    uint32_t descriptor_flags;
    os_handle_t handle;
    uint32_t open_flags;
    uint32_t resource_flags;
    uint32_t socket_family;
    uint32_t reserved;
    char path[OS_EXEC_FD_PATH_MAX];
} os_exec_fd_entry_t;

typedef struct os_exec_fd_map {
    os_versioned_header_t hdr;
    uint32_t count;
    uint32_t reserved;
    os_exec_fd_entry_t entries[OS_EXEC_FD_LIMIT];
} os_exec_fd_map_t;

enum os_process_state {
    OS_PROCESS_NEW = 0,
    OS_PROCESS_RUNNING = 1,
    OS_PROCESS_EXITING = 2,
    OS_PROCESS_DEAD = 3,
};

enum os_thread_state {
    OS_THREAD_NEW = 0,
    OS_THREAD_READY = 1,
    OS_THREAD_RUNNING = 2,
    OS_THREAD_BLOCKED = 3,
    OS_THREAD_STOPPED = 4,
    OS_THREAD_DEAD = 5,
};

/* PROCESS_WAIT currently implements the POSIX WNOHANG behavior. */
#define OS_PROCESS_WAIT_NOHANG (1U << 0)
