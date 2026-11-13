#pragma once

#include <kernel/base.h>
#include <kernel/spinlock.h>
#include <arch/x86_64/context.h>

struct file;
struct process;
struct thread;
/* 由进程注册表负责查找，并为调用方固定返回的线程。 */
struct thread *linux_thread_reference_by_tid(uint64_t tid);
uint64_t linux_thread_visible_tid(const struct thread *thread);
struct nv_frontend_file;
struct socket;
struct window_server_window;

/* 与对外公布的 Linux NOFILE 限额一致。即使只打开少量标签页，Chromium 数据库、共享映像和 IPC
 * 通道也会超过 256。 */
#define OS_LINUX_FD_LIMIT 1024U

enum linux_fd_kind {
    LINUX_FD_UNUSED = 0U,
    LINUX_FD_FILE = 1U,
    LINUX_FD_CONSOLE = 2U,
    LINUX_FD_NVIDIA = 3U,
    LINUX_FD_PROC = 4U,
    LINUX_FD_WINDOW = 5U,
    LINUX_FD_SOCKET = 6U,
    LINUX_FD_PIPE = 7U,
    LINUX_FD_EPOLL = 8U,
    LINUX_FD_EVENTFD = 9U,
    LINUX_FD_UNIX_SOCKET = 10U,
    LINUX_FD_NULL = 11U,
};

typedef struct linux_fd_entry {
    struct file *file;
    void *private_data;
    uint32_t flags;
    uint8_t kind;
    uint8_t reserved[3];
    struct linux_fd_description *description;
} linux_fd_entry_t;

typedef struct linux_fd_table {
    spinlock_t lock;
    linux_fd_entry_t entries[OS_LINUX_FD_LIMIT];
} linux_fd_table_t;

enum process_personality {
    OS_PROCESS_PERSONALITY_NATIVE = 0U,
    OS_PROCESS_PERSONALITY_LINUX_X86_64 = 1U,
};

void linux_fd_table_init(linux_fd_table_t *table);
void linux_fd_table_destroy(linux_fd_table_t *table);
kstatus_t linux_fd_table_clone(const linux_fd_table_t *source,
                               linux_fd_table_t *destination);
void linux_fd_table_close_on_exec(struct process *process);
/* 在表锁内重置，并将旧槽位的所有权转移给调用方。释放进程/表锁后，再释放快照。 */
void linux_fd_table_reset_stdio(linux_fd_table_t *table,
                                linux_fd_entry_t previous[3]);
void linux_fd_table_release_stdio(linux_fd_entry_t previous[3]);

/* 由进程生命周期代码调用；此逻辑与系统调用分发分开，避免 clone/set_tid_address 清理流程伪装成成功。 */
void linux_thread_exit_cleanup(struct thread *thread);

bool linux_process_is(struct process *process);
bool linux_elf_interpreter_path(const char *path);

int64_t linux_syscall_dispatch(arch_trap_frame_t *frame);

/* 第 7 阶段验收程序入口；仅由第 7 阶段测试构建调用。 */
bool linux_phase7_realtest(void);

/* 第 8 阶段验收程序入口；仅由第 8 阶段测试构建调用。 */
bool linux_phase8_realtest(void);

/* 第 9 阶段验收程序入口；仅由第 9 阶段测试构建调用。 */
bool linux_phase9_realtest(void);

/* 第 10 阶段验收程序入口；仅由第 10 阶段测试构建调用。 */
bool linux_phase10_realtest(void);

/* 第 11 阶段验收程序入口；仅由第 11 阶段测试构建调用。 */
bool linux_phase11_realtest(void);

/* 第 12 阶段验收程序入口；仅由第 12 阶段测试构建调用。 */
bool linux_phase12_realtest(void);

/* 第 13 阶段 Linux event/epoll 验收程序入口。 */
bool linux_phase13_realtest(void);
