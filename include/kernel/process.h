#pragma once
#include <kernel/sched.h>
#include <kernel/vm.h>
#include <kernel/handle.h>
#include <kernel/wait.h>
#include <uapi/process.h>
#include <kernel/linux_abi.h>
#pragma once
#include "base.h"
#include "object.h"
#include "handle.h"
#include "vm.h"
#include "list.h"
#include "spinlock.h"
#include <uapi/signal.h>

struct job;

/* Cold-path resource owners register independently; Process never names them. */
typedef void (*process_teardown_callback_t)(struct process *process);
#define PROCESS_TEARDOWN_CALLBACK_MAX 8U
#define PROCESS_CWD_CAPACITY 256U

enum process_state {
    PROCESS_NEW = 0,
    PROCESS_RUNNING,
    PROCESS_EXITING,
    PROCESS_DEAD,
};

typedef struct process {
    object_header_t object;
    uint64_t pid;
    atomic_uint state;
    uint32_t flags;

    vm_space_t *vm;
    handle_table_t handles;

    struct job *job;
    struct process *parent;

    /* The parent relation and child list keep process objects alive across
     * the interval between exit and waitpid. */
    spinlock_t parent_lock;
    spinlock_t children_lock;
    wait_queue_t child_waitq;
    list_head_t children;
    list_head_t child_node;

    spinlock_t thread_lock;
    list_head_t threads;
    /* Borrowed monitor hint; read/write and detach invalidation use thread_lock. */
    list_head_t *monitor_thread_cursor;
    /* 线程枚举缓存只在 thread_lock 内使用；线程成员变化时清空。 */
    list_head_t *enumeration_thread_cursor;
    uint32_t enumeration_thread_index;
    list_head_t job_node;
    uint32_t thread_count;
    uint64_t retired_cpu_ns; /* Thread-lock protected completed CPU time. */
    /* Linux presents the initial thread's TID as the process PID. */
    uint64_t linux_thread_group_leader_tid;

    /* Global monitor membership and stable user-visible metadata. */
    list_head_t global_node;
    /* 名称与当前目录都只在短暂持锁期间复制。 */
    spinlock_t info_lock;
    bool global_registered;
    char name[OS_PROCESS_NAME_MAX];

    int64_t exit_code;
    uint32_t exit_signal;
    uint64_t create_time_ns;

    /* Linux PT_INTERP images use a separate syscall-number personality. */
    uint32_t personality;
    /* The native System dynamic loader runs before Linux ABI activation. */
    bool linux_loader_pending;
    char linux_root[256];
    char cwd[PROCESS_CWD_CAPACITY];
    char linux_exe[256];
    vaddr_t linux_brk_base;
    vaddr_t linux_brk;
    linux_fd_table_t linux_fds;

    spinlock_t signal_lock;
    os_signal_action_t signal_actions[OS_SIGNAL_COUNT + 1U];
} process_t;

kstatus_t process_create(process_t *parent, process_t **out);
kstatus_t process_create_named(process_t *parent, const char *name,
                               process_t **out);
kstatus_t process_fork(process_t *parent, process_t **out);
kstatus_t process_abort(process_t *process);
kstatus_t process_wait_child(process_t *parent, int64_t pid, uint32_t options,
                             int64_t *child_pid, int64_t *exit_code,
                             uint32_t *exit_signal);
uint64_t process_parent_pid(const process_t *process);
kstatus_t process_enumerate(uint32_t index, os_process_snapshot_t *snapshot);
kstatus_t process_enumerate_thread(uint32_t index,
                                   os_thread_snapshot_t *snapshot);
kstatus_t process_task_query(uint64_t pid, uint64_t tid, os_task_info_t *info);
kstatus_t process_terminate_pid(process_t *caller, uint64_t pid, uint64_t create_time_ns);
kstatus_t process_terminate_tid(process_t *caller, uint64_t pid, uint64_t tid, uint64_t create_time_ns);
uint64_t process_allocate_task_id(void);
void process_set_name(process_t *process, const char *path);
process_t *current_process(void);
kstatus_t process_cwd_copy(process_t *process, char output[PROCESS_CWD_CAPACITY]);
kstatus_t process_cwd_set(process_t *process, const char *path);
kstatus_t process_exec(process_t *process, const char __user *path,
                       const char __user *const __user *argv,
                       const char __user *const __user *envp,
                       const os_exec_fd_map_t *descriptor_map);
void process_exec_debug_mark(uint32_t stage);
__noreturn void process_exit(int64_t status);
__noreturn void process_exit_signal(uint32_t signal);
kstatus_t thread_create_user(process_t *process, vaddr_t entry, vaddr_t stack_top,
                             vaddr_t fs_base, thread_t **out);
__noreturn void thread_exit(int64_t status);

/*
 * 对象类型和句柄权限属于内核 ABI，不暴露给用户态。句柄值本身始终保持不透明，
 * 系统调用只在查表时使用这些权限位。
 */
#define KOBJECT_TYPE_PROCESS       0x0101U
#define KOBJECT_TYPE_THREAD        0x0102U

/* WAIT 权限在所有可等待对象上使用同一位，系统调用可先做统一句柄校验。 */
#define OBJECT_RIGHT_WAIT          (1U << 31)

#define PROCESS_RIGHT_QUERY        (1U << 0)
#define PROCESS_RIGHT_CREATE_THREAD (1U << 1)
#define PROCESS_RIGHT_VM_OPERATION (1U << 2)
#define PROCESS_RIGHT_TERMINATE    (1U << 3)
#define PROCESS_RIGHT_WAIT         OBJECT_RIGHT_WAIT
#define PROCESS_RIGHT_ALL          (PROCESS_RIGHT_QUERY | PROCESS_RIGHT_CREATE_THREAD | \
                                    PROCESS_RIGHT_VM_OPERATION | PROCESS_RIGHT_TERMINATE | \
                                    PROCESS_RIGHT_WAIT)

#define THREAD_RIGHT_QUERY         (1U << 0)
#define THREAD_RIGHT_WAIT          OBJECT_RIGHT_WAIT
#define THREAD_RIGHT_TERMINATE     (1U << 2)
#define THREAD_RIGHT_ALL           (THREAD_RIGHT_QUERY | THREAD_RIGHT_WAIT | \
                                    THREAD_RIGHT_TERMINATE)

#define THREAD_FLAG_EXECUTION_REF  (1U << 0)
#define THREAD_FLAG_EXECUTION_REAPING (1U << 1)
#define THREAD_FLAG_TERMINATE_REQUEST (1U << 2)
#define PROCESS_FLAG_EVER_HAD_THREAD (1U << 0)
#define PROCESS_FLAG_RESOURCES_RELEASED (1U << 1)
#define PROCESS_FLAG_INIT_CPU_PINNED (1U << 2)

/* 系统调用先完成句柄写回，再用这组接口把新线程发布到运行队列。 */
kstatus_t thread_create_user_suspended(process_t *process, vaddr_t entry,
                                       vaddr_t stack_top, vaddr_t fs_base,
                                       uint64_t argument, thread_t **out);
kstatus_t thread_create_user_from_frame(process_t *process,
                                         const arch_trap_frame_t *frame,
                                         vaddr_t fs_base, thread_t **out);
kstatus_t thread_register_user_stack(thread_t *thread, vaddr_t base,
                                     size_t size);
kstatus_t thread_start(thread_t *thread);
kstatus_t thread_terminate(thread_t *thread, int64_t status);
/* Process-owned exit status publication followed by scheduler state change. */
bool process_publish_thread_exit(thread_t *thread, int64_t status);
void thread_release_execution_ref(thread_t *thread);
kstatus_t process_register_teardown_callback(
    process_teardown_callback_t callback);
uint32_t process_last_thread_create_stage(void);
bool process_core_self_test(void);
