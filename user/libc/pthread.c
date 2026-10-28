#include "libc/libc.h"

#include <limits.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>

#include <uapi/linux_abi.h>

#define PTHREAD_DEFAULT_STACK_SIZE (64U * 1024U)
#define PTHREAD_PAGE_SIZE 4096U
#define LIBC_TLS_HEADER_WORDS 6U
#define LIBC_TLS_MAGIC 0x4C4954454F53544CULL
#define LIBC_MUTEXATTR_TYPE_MASK 0x3U
#define LIBC_MUTEXATTR_PSHARED_BIT 0x4U
#define LIBC_CONDATTR_CLOCK_MASK 0xFFU
#define LIBC_CONDATTR_PSHARED_BIT 0x100U

_Static_assert(sizeof(pthread_mutexattr_t) == sizeof(uint32_t),
               "pthread_mutexattr_t must match glibc x86-64 ABI");
_Static_assert(sizeof(pthread_condattr_t) == sizeof(uint32_t),
               "pthread_condattr_t must match glibc x86-64 ABI");

typedef struct pthread_start_context {
    pthread_t thread;
    void *(*start_routine)(void *);
    void *argument;
    int error_number;
} pthread_start_context_t;

struct libc_pthread {
    os_handle_t handle;
    void *stack_address;
    size_t stack_size;
    bool owns_stack;
    volatile uint32_t lifecycle_lock;
    volatile bool joined;
    volatile bool detached;
    volatile bool exited;
    bool listed;
    bool cleanup;
    bool handle_ready;
    struct libc_pthread *detached_next;
    pthread_start_context_t *context;
    void *tls_mapping;
    size_t tls_mapping_size;
    uint64_t tls_fs_base;
    char name[16];
    void *specific[PTHREAD_KEYS_MAX];
    uint32_t specific_generation[PTHREAD_KEYS_MAX];
};

static void pthread_release_tls(pthread_t thread);

/* 保持 pthread_mutex_t 历史上的单字 ABI。递归所有权信息保存在进程内元数据中，
 * 并以公开互斥量地址作为键。 */
typedef struct pthread_mutex_metadata {
    pthread_mutex_t *mutex;
    int type;
    pthread_t owner;
    uint32_t depth;
    bool owner_valid;
    struct pthread_mutex_metadata *next;
} pthread_mutex_metadata_t;

#define MUTEX_METADATA_BUCKET_COUNT 1024U
#define LIBC_LOCK_STATE_FREE 0U
#define LIBC_LOCK_STATE_LOCKED 1U
#define LIBC_LOCK_STATE_CONTENDED 2U
/* 保留单字 ABI 和静态零初始化方式。只有跟踪所有权的互斥量需要旁路表；
 * 普通互斥量绝不能因旁路表分配器或无关的递归锁而串行等待。 */
#define LIBC_MUTEX_METADATA_BIT 0x80000000U
#define LIBC_MUTEX_SHARED_BIT 0x40000000U

static volatile uint64_t g_key_bitmap;
static void (*g_key_destructors[PTHREAD_KEYS_MAX])(void *);
/* 奇数代表示已分配，偶数代表示已删除。分配状态和标识必须在一个 atomic 字中发布，
 * 以支持无锁 TLS 访问。 */
static uint32_t g_key_generation[PTHREAD_KEYS_MAX];
static volatile uint32_t g_key_lock;
static void *g_main_specific[PTHREAD_KEYS_MAX];
static uint32_t g_main_specific_generation[PTHREAD_KEYS_MAX];
static pthread_start_context_t g_main_context;
static struct libc_pthread g_main_thread;
static volatile uint32_t g_detached_lock;
static pthread_t g_detached_threads;
static volatile uint32_t g_mutex_metadata_lock;
static pthread_mutex_metadata_t *
    g_mutex_metadata_buckets[MUTEX_METADATA_BUCKET_COUNT];

static void pthread_startup_debug_mark(const char *text, size_t length) {
    (void)__libc_linux_syscall6(OS_SYS_DEBUG_WRITE,
                                (uint64_t)(uintptr_t)text, length,
                                0U, 0U, 0U, 0U);
}

#define PTHREAD_ATFORK_MAX 64U
typedef struct pthread_atfork_entry {
    void (*prepare)(void);
    void (*parent)(void);
    void (*child)(void);
} pthread_atfork_entry_t;

static pthread_atfork_entry_t g_atfork_entries[PTHREAD_ATFORK_MAX];
static unsigned int g_atfork_count;
static volatile uint32_t g_atfork_lock;

static void detached_lock(void) {
    while (__atomic_exchange_n(&g_detached_lock, 1U, __ATOMIC_ACQUIRE) != 0U) {
        __asm__ volatile("pause");
    }
}

static void detached_unlock(void) {
    __atomic_store_n(&g_detached_lock, 0U, __ATOMIC_RELEASE);
}

static void atfork_lock(void) {
    while (__atomic_exchange_n(&g_atfork_lock, 1U, __ATOMIC_ACQUIRE) != 0U) {
        __asm__ volatile("pause");
    }
}

static void atfork_unlock(void) {
    __atomic_store_n(&g_atfork_lock, 0U, __ATOMIC_RELEASE);
}

static unsigned int atfork_snapshot(pthread_atfork_entry_t *entries) {
    unsigned int count;
    atfork_lock();
    count = g_atfork_count;
    if (count != 0U) {
        memcpy(entries, g_atfork_entries,
               count * sizeof(pthread_atfork_entry_t));
    }
    atfork_unlock();
    return count;
}

static void thread_lifecycle_lock(pthread_t thread) {
    while (__atomic_exchange_n(&thread->lifecycle_lock, 1U,
                               __ATOMIC_ACQUIRE) != 0U) {
        __asm__ volatile("pause");
    }
}

static void thread_lifecycle_unlock(pthread_t thread) {
    __atomic_store_n(&thread->lifecycle_lock, 0U, __ATOMIC_RELEASE);
}

/* 调用方必须持有 detached_lock()。 */
static void detached_list_add_locked(pthread_t thread) {
    if (thread == 0) return;
    thread_lifecycle_lock(thread);
    if (!thread->listed && !thread->cleanup) {
        thread->detached_next = g_detached_threads;
        g_detached_threads = thread;
        thread->listed = true;
    }
    thread_lifecycle_unlock(thread);
}

static void detached_list_add(pthread_t thread) {
    if (thread == 0) return;
    detached_lock();
    detached_list_add_locked(thread);
    detached_unlock();
}

/* 仅在内核发布 THREAD_DEAD 后回收已分离线程记录。 */
static void detached_reap(void) {
    pthread_t self = pthread_self();

    for (;;) {
        pthread_t candidate = 0;
        pthread_t *link = 0;
        os_wait_result_t wait_result = {0};
        int64_t status;

        detached_lock();
        for (pthread_t *entry = &g_detached_threads; *entry != 0;
             entry = &(*entry)->detached_next) {
            pthread_t thread = *entry;
            if (thread == self) {
                continue;
            }
            thread_lifecycle_lock(thread);
            if (!thread->handle_ready ||
                !__atomic_load_n(&thread->exited, __ATOMIC_ACQUIRE) ||
                thread->cleanup) {
                thread_lifecycle_unlock(thread);
                continue;
            }
            candidate = thread;
            link = entry;
            thread->cleanup = true;
            thread->listed = false;
            *link = thread->detached_next;
            thread->detached_next = 0;
            thread_lifecycle_unlock(thread);
            break;
        }
        detached_unlock();
        if (candidate == 0) return;

        status = __libc_native_syscall6(OS_SYS_WAIT_ONE, candidate->handle, 0U,
                                 (uint64_t)(uintptr_t)&wait_result,
                                 0U, 0U, 0U);
        if (status >= 0) {
            (void)__libc_native_syscall6(OS_SYS_HANDLE_CLOSE, candidate->handle,
                                  0U, 0U, 0U, 0U, 0U);
            pthread_release_tls(candidate);
            free(candidate->context);
            free(candidate);
            continue;
        }

        /* 退出标志可能早于内核状态发布；留待后续 API 调用时重试。 */
        detached_lock();
        thread_lifecycle_lock(candidate);
        candidate->cleanup = false;
        candidate->detached_next = g_detached_threads;
        g_detached_threads = candidate;
        candidate->listed = true;
        thread_lifecycle_unlock(candidate);
        detached_unlock();
        return;
    }
}

void __libc_reap_detached_threads(void) {
    detached_reap();
}

int __libc_thread_init(void) {
    uint64_t fs_base = 0U;
    pthread_startup_debug_mark("OS_LIBC_THREAD_INIT_ENTER\n",
                               sizeof("OS_LIBC_THREAD_INIT_ENTER\n") - 1U);
    g_main_context.thread = &g_main_thread;
    g_main_thread.context = &g_main_context;
    /* 动态链接器会在进入 CRT 入口前切换到 Linux 系统调用 ABI。首次设置 TLS 时使用
     * arch_prctl；否则原生 THREAD_CONTEXT 会被当作 Linux 系统调用解释。
     */
    int64_t probe = __libc_linux_syscall6(OS_SYS_DEBUG_QUERY, 0U, 0U, 0U, 0U,
                                          0U, 0U);
    if (probe == -38LL) {
        pthread_startup_debug_mark("OS_LIBC_THREAD_PROBE_LINUX\n",
                                   sizeof("OS_LIBC_THREAD_PROBE_LINUX\n") - 1U);
        int64_t get_status = __libc_linux_syscall6(
            LINUX_SYS_ARCH_PRCTL, LINUX_ARCH_GET_FS,
            (uint64_t)(uintptr_t)&fs_base, 0U, 0U, 0U, 0U);
        if (get_status < 0) {
            pthread_startup_debug_mark("OS_LIBC_THREAD_GETFS_FAIL\n",
                                       sizeof("OS_LIBC_THREAD_GETFS_FAIL\n") - 1U);
            return -1;
        }
        if (fs_base != 0U) {
            pthread_startup_debug_mark("OS_LIBC_THREAD_GETFS_OK\n",
                                       sizeof("OS_LIBC_THREAD_GETFS_OK\n") - 1U);
            uint64_t *tcb = (uint64_t *)(uintptr_t)fs_base;
            /* Chromium 的 local-exec TLS ABI 要求 fs:0 保存 TLS 自指针；
             * pthread_t 保存在 fs:32 的私有槽位中。 */
            tcb[0] = fs_base;
            tcb[4] = (uint64_t)(uintptr_t)&g_main_thread;
            return 0;
        }
        pthread_startup_debug_mark("OS_LIBC_THREAD_GETFS_ZERO\n",
                                   sizeof("OS_LIBC_THREAD_GETFS_ZERO\n") - 1U);
    }
    if (probe != -38LL) {
        pthread_startup_debug_mark("OS_LIBC_THREAD_PROBE_NATIVE\n",
                                   sizeof("OS_LIBC_THREAD_PROBE_NATIVE\n") - 1U);
    }
    int64_t status = probe == -38LL ?
        __libc_linux_syscall6(LINUX_SYS_ARCH_PRCTL, LINUX_ARCH_SET_FS,
                              (uint64_t)(uintptr_t)&g_main_thread, 0U, 0U, 0U, 0U) :
        __libc_native_syscall6(OS_SYS_THREAD_CONTEXT, OS_THREAD_CONTEXT_SET_FS,
        (uint64_t)(uintptr_t)&g_main_thread, 0U, 0U, 0U, 0U);
    pthread_startup_debug_mark(status < 0 ? "OS_LIBC_THREAD_SETFS_FAIL\n" :
                                      "OS_LIBC_THREAD_SETFS_OK\n",
                               status < 0 ? sizeof("OS_LIBC_THREAD_SETFS_FAIL\n") - 1U :
                                             sizeof("OS_LIBC_THREAD_SETFS_OK\n") - 1U);
    return status < 0 ? -1 : 0;
}

int *__libc_thread_errno_slot(void) {
    pthread_t thread = pthread_self();
    return thread == 0 ? &__libc_errno : &thread->context->error_number;
}

#define PTHREAD_RWLOCK_WRITER 0x80000000U
#define PTHREAD_RWLOCK_READERS 0x7fffffffU

static int pthread_invalid(void) {
    return EINVAL;
}

static int pthread_status_error(int64_t status) {
    int saved_errno;
    int error;
    if (status >= 0) return 0;
    /* pthread API 直接返回错误码，不得改写 errno。 */
    saved_errno = errno;
    (void)__libc_status_result(status);
    error = errno != 0 ? errno : EIO;
    errno = saved_errno;
    return error;
}

static bool pthread_linux_personality(void) {
    return __libc_linux_personality();
}

static uint64_t linux_futex_timeout_pointer(uint64_t timeout_ns,
                                            struct timespec *timeout) {
    if (timeout_ns == OS_WAIT_INFINITE || timeout == 0) return 0U;
    timeout->tv_sec = (time_t)(timeout_ns / 1000000000ULL);
    timeout->tv_nsec = (long)(timeout_ns % 1000000000ULL);
    return (uint64_t)(uintptr_t)timeout;
}

static int futex_wait_mode(volatile uint32_t *address, uint32_t expected,
                           uint64_t timeout_ns, bool shared) {
    if (pthread_linux_personality()) {
        struct timespec timeout;
        uint64_t timeout_pointer = linux_futex_timeout_pointer(
            timeout_ns, &timeout);
        return pthread_status_error(__libc_linux_syscall6(
            LINUX_SYS_FUTEX, (uint64_t)(uintptr_t)address,
            LINUX_FUTEX_WAIT | (shared ? 0U : LINUX_FUTEX_PRIVATE_FLAG), expected,
            timeout_pointer, 0U, 0U));
    }
    return pthread_status_error(__libc_native_syscall6(
        OS_SYS_FUTEX_WAIT, (uint64_t)(uintptr_t)address, expected,
        timeout_ns, shared ? OS_FUTEX_FLAG_SHARED : 0U, 0U, 0U));
}

static int futex_wait(volatile uint32_t *address, uint32_t expected,
                      uint64_t timeout_ns) {
    return futex_wait_mode(address, expected, timeout_ns, false);
}

static int futex_wake_mode(volatile uint32_t *address, uint32_t count, bool shared) {
    if (pthread_linux_personality()) {
        int64_t status = __libc_linux_syscall6(
            LINUX_SYS_FUTEX, (uint64_t)(uintptr_t)address,
            LINUX_FUTEX_WAKE | (shared ? 0U : LINUX_FUTEX_PRIVATE_FLAG), count,
            0U, 0U, 0U);
        return status < 0 ? pthread_status_error(status) : 0;
    }
    int64_t status = __libc_native_syscall6(OS_SYS_FUTEX_WAKE,
                                     (uint64_t)(uintptr_t)address, count,
                                     shared ? OS_FUTEX_FLAG_SHARED : 0U, 0U, 0U, 0U);
    return status < 0 ? pthread_status_error(status) : 0;
}

static int futex_wake(volatile uint32_t *address, uint32_t count) {
    return futex_wake_mode(address, count, false);
}

static void key_lock(void) {
    while (__atomic_exchange_n(&g_key_lock, 1U, __ATOMIC_ACQUIRE) != 0U) {
        __asm__ volatile("pause");
    }
}

static void key_unlock(void) {
    __atomic_store_n(&g_key_lock, 0U, __ATOMIC_RELEASE);
}

static bool key_is_valid_locked(pthread_key_t key) {
    return key < PTHREAD_KEYS_MAX &&
           ((__atomic_load_n(&g_key_bitmap, __ATOMIC_RELAXED) >> key) & 1U) != 0U;
}

static void run_key_destructors(pthread_t thread) {
    void **values = thread == 0 ? g_main_specific : thread->specific;
    uint32_t *generations = thread == 0 ? g_main_specific_generation :
                                         thread->specific_generation;
    for (unsigned int pass = 0U; pass < PTHREAD_DESTRUCTOR_ITERATIONS; ++pass) {
        bool called = false;
        for (pthread_key_t key = 0U; key < PTHREAD_KEYS_MAX; ++key) {
            void *value = 0;
            void (*destructor)(void *) = 0;

            /* 在键锁保护下获取一个槽位的快照，再于解锁后调用用户代码，
             * 以便析构函数可以使用键 API。 */
            key_lock();
            if (key_is_valid_locked(key) &&
                __atomic_load_n(&generations[key], __ATOMIC_ACQUIRE) ==
                    __atomic_load_n(&g_key_generation[key], __ATOMIC_ACQUIRE) &&
                __atomic_load_n(&values[key], __ATOMIC_RELAXED) != 0) {
                value = __atomic_load_n(&values[key], __ATOMIC_RELAXED);
                __atomic_store_n(&values[key], 0, __ATOMIC_RELAXED);
                __atomic_store_n(&generations[key], 0U, __ATOMIC_RELEASE);
                destructor = g_key_destructors[key];
            }
            key_unlock();
            if (destructor != 0) {
                called = true;
                destructor(value);
            }
        }
        if (!called) return;
    }
}

static size_t page_round(size_t size) {
    if (size == 0U || size > SIZE_MAX - (PTHREAD_PAGE_SIZE - 1U)) return 0U;
    return (size + PTHREAD_PAGE_SIZE - 1U) &
           ~(size_t)(PTHREAD_PAGE_SIZE - 1U);
}

static bool pthread_tls_metadata(uint64_t fs_base, uint64_t *tls_size,
                                 uint64_t *template_base,
                                 uint64_t *tls_alignment) {
    const uint64_t maximum = 1ULL << 20U;
    uint64_t *tcb;
    if (fs_base == 0U || fs_base > UINTPTR_MAX -
        LIBC_TLS_HEADER_WORDS * sizeof(uint64_t) ||
        tls_size == 0 || template_base == 0 || tls_alignment == 0) {
        return false;
    }
    tcb = (uint64_t *)(uintptr_t)fs_base;
    if (tcb[3] != LIBC_TLS_MAGIC || tcb[1] == 0U || tcb[1] > maximum ||
        tcb[2] == 0U || tcb[5] == 0U || tcb[5] > maximum ||
        (tcb[5] & (tcb[5] - 1U)) != 0U) return false;
    *tls_size = tcb[1];
    *template_base = tcb[2];
    *tls_alignment = tcb[5];
    return true;
}

static bool pthread_prepare_tls(pthread_t thread) {
    uint64_t fs_base = 0U;
    uint64_t tls_size = 0U;
    uint64_t template_base = 0U;
    uint64_t tls_alignment = 0U;
    uint64_t aligned_tls_size;
    uint64_t tls_mapping_offset;
    size_t mapping_size;
    size_t required_size;
    void *mapping;
    uint64_t child_tls_base;
    uint64_t child_fs;
    int64_t status = __libc_linux_syscall6(
        LINUX_SYS_ARCH_PRCTL, LINUX_ARCH_GET_FS,
        (uint64_t)(uintptr_t)&fs_base, 0U, 0U, 0U, 0U);
    if (status < 0) return false;
    if (!pthread_tls_metadata(fs_base, &tls_size, &template_base,
                             &tls_alignment)) return true;
    if (tls_size > UINT64_MAX - (tls_alignment - 1U)) {
        errno = EOVERFLOW;
        return false;
    }
    aligned_tls_size = (tls_size + tls_alignment - 1U) &
                       ~(tls_alignment - 1U);
    if (aligned_tls_size > SIZE_MAX - (tls_alignment - 1U) ||
        aligned_tls_size + tls_alignment - 1U > SIZE_MAX -
            LIBC_TLS_HEADER_WORDS * sizeof(uint64_t)) {
        errno = EOVERFLOW;
        return false;
    }
    tls_mapping_offset = tls_alignment - 1U;
    required_size = (size_t)aligned_tls_size + (size_t)tls_mapping_offset +
                    LIBC_TLS_HEADER_WORDS * sizeof(uint64_t);
    mapping_size = page_round(required_size);
    if (mapping_size == 0U) {
        errno = EOVERFLOW;
        return false;
    }
    mapping = mmap(0, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return false;
    child_tls_base = ((uint64_t)(uintptr_t)mapping + tls_mapping_offset) &
                     ~(tls_alignment - 1U);
    memcpy((void *)(uintptr_t)child_tls_base,
           (const void *)(uintptr_t)template_base, (size_t)tls_size);
    child_fs = child_tls_base + aligned_tls_size;
    {
        uint64_t *tcb = (uint64_t *)(uintptr_t)child_fs;
        tcb[0] = child_fs;
        tcb[1] = tls_size;
        tcb[2] = template_base;
        tcb[3] = LIBC_TLS_MAGIC;
        tcb[4] = (uint64_t)(uintptr_t)thread;
        tcb[5] = tls_alignment;
    }
    thread->tls_mapping = mapping;
    thread->tls_mapping_size = mapping_size;
    thread->tls_fs_base = child_fs;
    return true;
}

static void pthread_release_tls(pthread_t thread) {
    if (thread == 0 || thread->tls_mapping == 0 ||
        thread->tls_mapping_size == 0U) return;
    (void)munmap(thread->tls_mapping, thread->tls_mapping_size);
    thread->tls_mapping = 0;
    thread->tls_mapping_size = 0U;
    thread->tls_fs_base = 0U;
}

static int absolute_timeout(const struct timespec *absolute,
                            clockid_t clock_id, uint64_t *timeout_ns) {
    struct timespec now;
    uint64_t seconds;
    uint64_t nanoseconds;
    if (absolute == 0 || timeout_ns == 0 || absolute->tv_nsec < 0 ||
        absolute->tv_nsec >= 1000000000L) return EINVAL;
    if (clock_gettime(clock_id, &now) < 0) return errno;
    if (absolute->tv_sec < now.tv_sec ||
        (absolute->tv_sec == now.tv_sec &&
         absolute->tv_nsec <= now.tv_nsec)) {
        *timeout_ns = 0U;
        return 0;
    }
    seconds = (uint64_t)(absolute->tv_sec - now.tv_sec);
    if (absolute->tv_nsec < now.tv_nsec) {
        --seconds;
        nanoseconds = (uint64_t)absolute->tv_nsec + 1000000000ULL -
                      (uint64_t)now.tv_nsec;
    } else {
        nanoseconds = (uint64_t)absolute->tv_nsec -
                      (uint64_t)now.tv_nsec;
    }
    if (seconds > (UINT64_MAX - nanoseconds) / 1000000000ULL) {
        *timeout_ns = UINT64_MAX;
    } else {
        *timeout_ns = seconds * 1000000000ULL + nanoseconds;
    }
    return 0;
}

int pthread_attr_init(pthread_attr_t *attribute) {
    if (attribute == 0) return pthread_invalid();
    attribute->stack_address = 0;
    attribute->stack_size = PTHREAD_DEFAULT_STACK_SIZE;
    attribute->guard_size = 0U;
    attribute->detach_state = PTHREAD_CREATE_JOINABLE;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attribute) {
    return attribute == 0 ? pthread_invalid() : 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attribute, size_t size) {
    if (attribute == 0 || size < PTHREAD_STACK_MIN) return pthread_invalid();
    if (page_round(size) == 0U) return EOVERFLOW;
    attribute->stack_address = 0;
    attribute->stack_size = size;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attribute, size_t *size) {
    if (attribute == 0 || size == 0) return pthread_invalid();
    *size = attribute->stack_size;
    return 0;
}

int pthread_attr_setguardsize(pthread_attr_t *attribute, size_t size) {
    size_t rounded;
    if (attribute == 0) return pthread_invalid();
    if (size == 0U) {
        attribute->guard_size = 0U;
        return 0;
    }
    rounded = page_round(size);
    if (rounded == 0U) return EOVERFLOW;
    attribute->guard_size = rounded;
    return 0;
}

int pthread_attr_getguardsize(const pthread_attr_t *attribute, size_t *size) {
    if (attribute == 0 || size == 0) return pthread_invalid();
    *size = attribute->guard_size;
    return 0;
}

int pthread_attr_setstack(pthread_attr_t *attribute, void *address,
                          size_t size) {
    if (attribute == 0 || address == 0 || size < PTHREAD_STACK_MIN ||
        (uintptr_t)address > UINTPTR_MAX - size) return pthread_invalid();
    attribute->stack_address = address;
    attribute->stack_size = size;
    return 0;
}

int pthread_attr_getstack(const pthread_attr_t *attribute, void **address,
                          size_t *size) {
    if (attribute == 0 || address == 0 || size == 0) return pthread_invalid();
    *address = attribute->stack_address;
    *size = attribute->stack_size;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attribute, int state) {
    if (attribute == 0 ||
        (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED)) {
        return pthread_invalid();
    }
    attribute->detach_state = state;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attribute, int *state) {
    if (attribute == 0 || state == 0) return pthread_invalid();
    *state = attribute->detach_state;
    return 0;
}

static void mutex_metadata_lock(void) {
    bool contended = false;
    for (;;) {
        uint32_t expected = LIBC_LOCK_STATE_FREE;
        uint32_t desired = contended ? LIBC_LOCK_STATE_CONTENDED :
                                       LIBC_LOCK_STATE_LOCKED;
        if (__atomic_compare_exchange_n(&g_mutex_metadata_lock, &expected,
                                        desired,
                                        false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            return;
        }

        /* 状态 2 表示所有者释放锁时必须唤醒一个休眠者。被唤醒的等待者接管锁后仍保留
         * 此状态，避免一串等待者滞留在看似无竞争的解锁操作之后。 */
        if (expected == LIBC_LOCK_STATE_LOCKED) {
            expected = LIBC_LOCK_STATE_LOCKED;
            if (!__atomic_compare_exchange_n(
                    &g_mutex_metadata_lock, &expected,
                    LIBC_LOCK_STATE_CONTENDED, false, __ATOMIC_ACQ_REL,
                    __ATOMIC_RELAXED)) {
                contended = true;
                continue;
            }
        } else if (expected != LIBC_LOCK_STATE_CONTENDED) {
            contended = true;
            continue;
        }
        contended = true;
        (void)futex_wait(&g_mutex_metadata_lock,
                         LIBC_LOCK_STATE_CONTENDED, OS_WAIT_INFINITE);
    }
}

static void mutex_metadata_unlock(void) {
    uint32_t previous_state = __atomic_exchange_n(
        &g_mutex_metadata_lock, LIBC_LOCK_STATE_FREE, __ATOMIC_RELEASE);
    if (previous_state == LIBC_LOCK_STATE_CONTENDED) {
        (void)futex_wake(&g_mutex_metadata_lock, 1U);
    }
}

static uint32_t mutex_metadata_bucket_index(const pthread_mutex_t *mutex) {
    uintptr_t value = (uintptr_t)mutex >> 3U;
    value ^= value >> 13U;
    value ^= value >> 23U;
    return (uint32_t)value & (MUTEX_METADATA_BUCKET_COUNT - 1U);
}

/* 调用方必须持有 g_mutex_metadata_lock。 */
static pthread_mutex_metadata_t *mutex_metadata_find_locked(
    pthread_mutex_t *mutex) {
    uint32_t bucket = mutex_metadata_bucket_index(mutex);
    for (pthread_mutex_metadata_t *entry = g_mutex_metadata_buckets[bucket];
         entry != 0;
         entry = entry->next) {
        if (entry->mutex == mutex) return entry;
    }
    return 0;
}

/* 调用方必须持有 g_mutex_metadata_lock。 */
static pthread_mutex_metadata_t *mutex_metadata_remove_locked(
    pthread_mutex_t *mutex) {
    uint32_t bucket = mutex_metadata_bucket_index(mutex);
    pthread_mutex_metadata_t **link = &g_mutex_metadata_buckets[bucket];
    while (*link != 0) {
        pthread_mutex_metadata_t *entry = *link;
        if (entry->mutex == mutex) {
            *link = entry->next;
            entry->next = 0;
            return entry;
        }
        link = &entry->next;
    }
    return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attribute) {
    if (attribute == 0) return pthread_invalid();
    attribute->value = 0U;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attribute) {
    return attribute == 0 ? pthread_invalid() : 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attribute, int type) {
    if (attribute == 0) return pthread_invalid();
    if (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_RECURSIVE &&
        type != PTHREAD_MUTEX_ERRORCHECK) return EINVAL;
    attribute->value = (attribute->value & ~LIBC_MUTEXATTR_TYPE_MASK) |
                       ((uint32_t)type & LIBC_MUTEXATTR_TYPE_MASK);
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attribute, int *type) {
    if (attribute == 0 || type == 0) return pthread_invalid();
    *type = (int)(attribute->value & LIBC_MUTEXATTR_TYPE_MASK);
    return 0;
}

int pthread_mutexattr_setpshared(pthread_mutexattr_t *attribute, int shared) {
    if (attribute == 0) return pthread_invalid();
    if (shared != PTHREAD_PROCESS_PRIVATE && shared != PTHREAD_PROCESS_SHARED) {
        return EINVAL;
    }
    attribute->value = (attribute->value & ~LIBC_MUTEXATTR_PSHARED_BIT) |
        (shared == PTHREAD_PROCESS_SHARED ? LIBC_MUTEXATTR_PSHARED_BIT : 0U);
    return 0;
}

int pthread_mutexattr_getpshared(const pthread_mutexattr_t *attribute,
                                 int *shared) {
    if (attribute == 0 || shared == 0) return pthread_invalid();
    *shared = (attribute->value & LIBC_MUTEXATTR_PSHARED_BIT) != 0U ?
              PTHREAD_PROCESS_SHARED : PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_mutexattr_setprotocol(pthread_mutexattr_t *attribute,
                                  int protocol) {
    if (attribute == 0) return pthread_invalid();
    if (protocol != 0 && protocol != PTHREAD_PRIO_INHERIT) return EINVAL;
    /* 当前 System futex/互斥锁 ABI 不包含优先级继承状态。 */
    return protocol == PTHREAD_PRIO_INHERIT ? ENOTSUP : 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex,
                       const pthread_mutexattr_t *attribute) {
    pthread_mutex_metadata_t *entry;
    pthread_mutex_metadata_t *removed = 0;
    uint32_t attribute_value = attribute == 0 ? 0U : attribute->value;
    int type = (int)(attribute_value & LIBC_MUTEXATTR_TYPE_MASK);
    int process_shared = (attribute_value & LIBC_MUTEXATTR_PSHARED_BIT) != 0U ?
                         PTHREAD_PROCESS_SHARED : PTHREAD_PROCESS_PRIVATE;
    if (mutex == 0 ||
        (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_RECURSIVE &&
         type != PTHREAD_MUTEX_ERRORCHECK)) {
        return pthread_invalid();
    }
    if (process_shared == PTHREAD_PROCESS_SHARED &&
        type != PTHREAD_MUTEX_NORMAL) return ENOTSUP;

    /* 元数据仅属于此 libc 映像，因此公开互斥锁布局仍兼容原有的单字状态和静态初始化器。
     *
     * pthread_mutex_init() 接收的存储区域中，原有字节不属于新互斥锁。特别是 Chromium
     * 会将此不透明对象嵌入 C++ 类中，而不会预先将其清零。不得把传入的状态字视为
     * 正在使用的锁并返回 EBUSY；后续初始化必须覆盖该状态。 */
    mutex_metadata_lock();
    entry = mutex_metadata_find_locked(mutex);
    if (type == PTHREAD_MUTEX_RECURSIVE || type == PTHREAD_MUTEX_ERRORCHECK) {
        if (entry == 0) {
            entry = (pthread_mutex_metadata_t *)malloc(sizeof(*entry));
            if (entry == 0) {
                mutex_metadata_unlock();
                return ENOMEM;
            }
            entry->mutex = mutex;
            uint32_t bucket = mutex_metadata_bucket_index(mutex);
            entry->next = g_mutex_metadata_buckets[bucket];
            g_mutex_metadata_buckets[bucket] = entry;
        }
        entry->type = type;
        entry->owner = 0;
        entry->depth = 0U;
        entry->owner_valid = false;
    } else if (entry != 0) {
        removed = mutex_metadata_remove_locked(mutex);
    }
    __atomic_store_n(&mutex->state,
        (type == PTHREAD_MUTEX_RECURSIVE || type == PTHREAD_MUTEX_ERRORCHECK) ?
            LIBC_MUTEX_METADATA_BIT :
            (process_shared == PTHREAD_PROCESS_SHARED ? LIBC_MUTEX_SHARED_BIT : LIBC_LOCK_STATE_FREE),
        __ATOMIC_RELEASE);
    mutex_metadata_unlock();
    free(removed);
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    pthread_mutex_metadata_t *removed;
    if (mutex == 0) return pthread_invalid();
    mutex_metadata_lock();
    if ((__atomic_load_n(&mutex->state, __ATOMIC_ACQUIRE) &
         ~(LIBC_MUTEX_METADATA_BIT | LIBC_MUTEX_SHARED_BIT)) != LIBC_LOCK_STATE_FREE) {
        mutex_metadata_unlock();
        return EBUSY;
    }
    removed = mutex_metadata_remove_locked(mutex);
    mutex_metadata_unlock();
    free(removed);
    return 0;
}

static int mutex_try_acquire(pthread_mutex_t *mutex, bool try_only,
                             uint32_t acquired_state) {
    pthread_mutex_metadata_t *entry;
    pthread_t self = 0;
    uint32_t expected;
    int result;

    if ((__atomic_load_n(&mutex->state, __ATOMIC_ACQUIRE) &
         LIBC_MUTEX_METADATA_BIT) == 0U) {
        expected = __atomic_load_n(&mutex->state, __ATOMIC_RELAXED) & LIBC_MUTEX_SHARED_BIT;
        return __atomic_compare_exchange_n(&mutex->state, &expected,
            acquired_state | expected, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ?
                0 : EBUSY;
    }
    expected = LIBC_MUTEX_METADATA_BIT;
    mutex_metadata_lock();
    entry = mutex_metadata_find_locked(mutex);
    if (entry != 0 &&
        (entry->type == PTHREAD_MUTEX_RECURSIVE ||
         entry->type == PTHREAD_MUTEX_ERRORCHECK)) {
        self = pthread_self();
        if (entry->owner_valid && entry->owner == self) {
            if (entry->type == PTHREAD_MUTEX_ERRORCHECK) {
                result = try_only ? EBUSY : EDEADLK;
            } else if (entry->depth == UINT32_MAX) {
                result = EAGAIN;
            } else {
                ++entry->depth;
                result = 0;
            }
            mutex_metadata_unlock();
            return result;
        }
    }
    if (!__atomic_compare_exchange_n(&mutex->state, &expected,
                                     acquired_state | LIBC_MUTEX_METADATA_BIT,
                                     false, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED)) {
        mutex_metadata_unlock();
        return EBUSY;
    }
    if (entry != 0 &&
        (entry->type == PTHREAD_MUTEX_RECURSIVE ||
         entry->type == PTHREAD_MUTEX_ERRORCHECK)) {
        entry->owner = self;
        entry->owner_valid = true;
        entry->depth = 1U;
    }
    mutex_metadata_unlock();
    return 0;
}

static int mutex_wait_contended(pthread_mutex_t *mutex, uint64_t timeout_ns) {
    for (;;) {
        uint32_t state = __atomic_load_n(&mutex->state, __ATOMIC_ACQUIRE);
        uint32_t metadata = state & (LIBC_MUTEX_METADATA_BIT | LIBC_MUTEX_SHARED_BIT);
        uint32_t lock_state = state & ~(LIBC_MUTEX_METADATA_BIT | LIBC_MUTEX_SHARED_BIT);
        if (lock_state == LIBC_LOCK_STATE_FREE) return EAGAIN;
        if (lock_state == LIBC_LOCK_STATE_LOCKED) {
            uint32_t expected = state;
            if (!__atomic_compare_exchange_n(
                    &mutex->state, &expected,
                    metadata | LIBC_LOCK_STATE_CONTENDED,
                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                continue;
            }
        } else if (lock_state != LIBC_LOCK_STATE_CONTENDED) {
            return EINVAL;
        }
        return futex_wait_mode(&mutex->state, metadata | LIBC_LOCK_STATE_CONTENDED,
                               timeout_ns, (metadata & LIBC_MUTEX_SHARED_BIT) != 0U);
    }
}

static int mutex_lock_until(pthread_mutex_t *mutex, uint64_t timeout_ns,
                            bool timed) {
    bool contended = false;
    for (;;) {
        int error = mutex_try_acquire(
            mutex, false, contended ? LIBC_LOCK_STATE_CONTENDED :
                                      LIBC_LOCK_STATE_LOCKED);
        if (error == 0 || error == EAGAIN) return error;
        if (error != EBUSY) return error;
        error = mutex_wait_contended(
            mutex, timed ? timeout_ns : OS_WAIT_INFINITE);
        contended = true;
        if (error == EAGAIN) continue;
        if (error != 0) return error;
    }
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    return mutex == 0 ? pthread_invalid() : mutex_lock_until(mutex, 0U, false);
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    if (mutex == 0) return pthread_invalid();
    return mutex_try_acquire(mutex, true, LIBC_LOCK_STATE_LOCKED);
}

int pthread_mutex_timedlock(pthread_mutex_t *mutex,
                            const struct timespec *abstime) {
    uint64_t timeout_ns;
    bool contended = false;
    int error;
    if (mutex == 0) return pthread_invalid();
    error = absolute_timeout(abstime, CLOCK_REALTIME, &timeout_ns);
    if (error != 0) return error;
    for (;;) {
        error = mutex_try_acquire(
            mutex, false, contended ? LIBC_LOCK_STATE_CONTENDED :
                                      LIBC_LOCK_STATE_LOCKED);
        if (error == 0 || error == EAGAIN) return error;
        if (error != EBUSY) return error;
        error = mutex_wait_contended(mutex, timeout_ns);
        contended = true;
        if (error == EAGAIN) {
            error = absolute_timeout(abstime, CLOCK_REALTIME, &timeout_ns);
            if (error != 0) return error;
            continue;
        }
        if (error != 0) return error;
    }
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    pthread_mutex_metadata_t *entry;
    pthread_t self;
    uint32_t previous_state;
    if (mutex == 0) return pthread_invalid();

    if ((__atomic_load_n(&mutex->state, __ATOMIC_ACQUIRE) &
         LIBC_MUTEX_METADATA_BIT) == 0U) {
        uint32_t shared = __atomic_load_n(&mutex->state, __ATOMIC_RELAXED) & LIBC_MUTEX_SHARED_BIT;
        previous_state = __atomic_exchange_n(
            &mutex->state, shared, __ATOMIC_RELEASE);
        if ((previous_state & ~LIBC_MUTEX_SHARED_BIT) == LIBC_LOCK_STATE_FREE) return EPERM;
        return (previous_state & ~LIBC_MUTEX_SHARED_BIT) == LIBC_LOCK_STATE_CONTENDED ?
            futex_wake_mode(&mutex->state, 1U, shared != 0U) : 0;
    }
    mutex_metadata_lock();
    entry = mutex_metadata_find_locked(mutex);
    if (entry != 0 &&
        (entry->type == PTHREAD_MUTEX_RECURSIVE ||
         entry->type == PTHREAD_MUTEX_ERRORCHECK)) {
        self = pthread_self();
        if (!entry->owner_valid || entry->owner != self) {
            mutex_metadata_unlock();
            return EPERM;
        }
        if (entry->type == PTHREAD_MUTEX_RECURSIVE && entry->depth > 1U) {
            --entry->depth;
            mutex_metadata_unlock();
            return 0;
        }
        entry->owner = 0;
        entry->depth = 0U;
        entry->owner_valid = false;
        previous_state = __atomic_exchange_n(
            &mutex->state, LIBC_MUTEX_METADATA_BIT, __ATOMIC_RELEASE);
        mutex_metadata_unlock();
        return previous_state == (LIBC_MUTEX_METADATA_BIT | LIBC_LOCK_STATE_CONTENDED) ?
            futex_wake(&mutex->state, 1U) : 0;
    }
    mutex_metadata_unlock();
    previous_state = __atomic_exchange_n(
        &mutex->state, LIBC_LOCK_STATE_FREE, __ATOMIC_RELEASE);
    if (previous_state == LIBC_LOCK_STATE_FREE) {
        return EPERM;
    }
    return previous_state == LIBC_LOCK_STATE_CONTENDED ?
        futex_wake(&mutex->state, 1U) : 0;
}

int pthread_condattr_init(pthread_condattr_t *attribute) {
    if (attribute == 0) return pthread_invalid();
    attribute->value = (uint32_t)CLOCK_REALTIME;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attribute) {
    return attribute == 0 ? pthread_invalid() : 0;
}

int pthread_condattr_setclock(pthread_condattr_t *attribute, clockid_t clock_id) {
    if (attribute == 0) return pthread_invalid();
    if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC) return EINVAL;
    attribute->value = (attribute->value & ~LIBC_CONDATTR_CLOCK_MASK) |
                       ((uint32_t)clock_id & LIBC_CONDATTR_CLOCK_MASK);
    return 0;
}

int pthread_condattr_getclock(const pthread_condattr_t *attribute,
                              clockid_t *clock_id) {
    if (attribute == 0 || clock_id == 0) return pthread_invalid();
    *clock_id = (clockid_t)(attribute->value & LIBC_CONDATTR_CLOCK_MASK);
    return 0;
}

int pthread_condattr_setpshared(pthread_condattr_t *attribute, int shared) {
    if (attribute == 0) return pthread_invalid();
    if (shared != PTHREAD_PROCESS_PRIVATE && shared != PTHREAD_PROCESS_SHARED) {
        return EINVAL;
    }
    attribute->value = (attribute->value & ~LIBC_CONDATTR_PSHARED_BIT) |
        (shared == PTHREAD_PROCESS_SHARED ? LIBC_CONDATTR_PSHARED_BIT : 0U);
    return 0;
}

int pthread_condattr_getpshared(const pthread_condattr_t *attribute,
                                int *shared) {
    if (attribute == 0 || shared == 0) return pthread_invalid();
    *shared = (attribute->value & LIBC_CONDATTR_PSHARED_BIT) != 0U ?
              PTHREAD_PROCESS_SHARED : PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_cond_init(pthread_cond_t *condition,
                      const pthread_condattr_t *attribute) {
    uint32_t attribute_value = attribute == 0 ?
        (uint32_t)CLOCK_REALTIME : attribute->value;
    uint32_t clock_id = attribute_value & LIBC_CONDATTR_CLOCK_MASK;
    if (condition == 0 ||
        (attribute_value & ~(LIBC_CONDATTR_CLOCK_MASK |
                             LIBC_CONDATTR_PSHARED_BIT)) != 0U ||
        (clock_id != (uint32_t)CLOCK_REALTIME &&
         clock_id != (uint32_t)CLOCK_MONOTONIC)) {
        return pthread_invalid();
    }
    __atomic_store_n(&condition->sequence, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&condition->clock_id, clock_id, __ATOMIC_RELAXED);
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *condition) {
    return condition == 0 ? pthread_invalid() : 0;
}

/* POSIX 要求条件变量等待完全释放互斥锁。将递归深度保存在私有元数据中，并在等待后
 * 恢复，以确保递归互斥锁在等待前后的所有权约定一致。 */
static int mutex_release_for_condition(pthread_mutex_t *mutex,
                                       uint32_t *depth) {
    pthread_mutex_metadata_t *entry;
    pthread_t self;
    uint32_t state;

    if (depth == 0) return pthread_invalid();
    *depth = 1U;
    if ((__atomic_load_n(&mutex->state, __ATOMIC_ACQUIRE) &
         LIBC_MUTEX_METADATA_BIT) == 0U) {
        return pthread_mutex_unlock(mutex);
    }
    mutex_metadata_lock();
    entry = mutex_metadata_find_locked(mutex);
    if (entry != 0 &&
        (entry->type == PTHREAD_MUTEX_RECURSIVE ||
         entry->type == PTHREAD_MUTEX_ERRORCHECK)) {
        self = pthread_self();
        if (!entry->owner_valid || entry->owner != self || entry->depth == 0U) {
            mutex_metadata_unlock();
            return EPERM;
        }
        *depth = entry->type == PTHREAD_MUTEX_RECURSIVE ? entry->depth : 1U;
        entry->owner = 0;
        entry->depth = 0U;
        entry->owner_valid = false;
        state = __atomic_exchange_n(&mutex->state, LIBC_MUTEX_METADATA_BIT,
                                    __ATOMIC_RELEASE);
        mutex_metadata_unlock();
        return state == (LIBC_MUTEX_METADATA_BIT | LIBC_LOCK_STATE_CONTENDED) ?
            futex_wake(&mutex->state, 1U) : 0;
    }
    state = __atomic_exchange_n(&mutex->state, LIBC_LOCK_STATE_FREE,
                                __ATOMIC_RELEASE);
    mutex_metadata_unlock();
    if (state == LIBC_LOCK_STATE_FREE) return EPERM;
    return state == LIBC_LOCK_STATE_CONTENDED ?
        futex_wake(&mutex->state, 1U) : 0;
}

static int mutex_restore_after_condition(pthread_mutex_t *mutex,
                                         uint32_t depth) {
    if (depth == 0U) return EINVAL;
    for (uint32_t level = 0U; level < depth; ++level) {
        int error = pthread_mutex_lock(mutex);
        if (error != 0) return error;
    }
    return 0;
}

static int condition_wait(pthread_cond_t *condition, pthread_mutex_t *mutex,
                          uint64_t timeout_ns) {
    uint32_t sequence;
    uint32_t depth;
    int error;
    int restore_error;
    if (condition == 0 || mutex == 0) return pthread_invalid();
    sequence = __atomic_load_n(&condition->sequence, __ATOMIC_ACQUIRE);
    error = mutex_release_for_condition(mutex, &depth);
    if (error != 0) return error;
    error = futex_wait(&condition->sequence, sequence, timeout_ns);
    if (error == EAGAIN) error = 0;
    restore_error = mutex_restore_after_condition(mutex, depth);
    if (error == 0 && restore_error != 0) error = restore_error;
    return error;
}

int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex) {
    return condition_wait(condition, mutex, OS_WAIT_INFINITE);
}

int pthread_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex,
                           const struct timespec *abstime) {
    uint64_t timeout_ns;
    int error;
    if (condition == 0 || mutex == 0) return pthread_invalid();
    uint32_t clock_id = __atomic_load_n(&condition->clock_id,
                                        __ATOMIC_ACQUIRE);
    if (clock_id != (uint32_t)CLOCK_REALTIME &&
        clock_id != (uint32_t)CLOCK_MONOTONIC) return EINVAL;
    error = absolute_timeout(abstime, (clockid_t)clock_id, &timeout_ns);
    if (error != 0) return error;
    return condition_wait(condition, mutex, timeout_ns);
}

int pthread_cond_signal(pthread_cond_t *condition) {
    if (condition == 0) return pthread_invalid();
    (void)__atomic_add_fetch(&condition->sequence, 1U, __ATOMIC_RELEASE);
    return futex_wake(&condition->sequence, 1U);
}

int pthread_cond_broadcast(pthread_cond_t *condition) {
    if (condition == 0) return pthread_invalid();
    (void)__atomic_add_fetch(&condition->sequence, 1U, __ATOMIC_RELEASE);
    return futex_wake(&condition->sequence, UINT32_MAX);
}

int pthread_once(pthread_once_t *once, void (*function)(void)) {
    if (once == 0 || function == 0) return pthread_invalid();
    for (;;) {
        uint32_t state = __atomic_load_n(&once->state, __ATOMIC_ACQUIRE);
        if (state == 2U) return 0;
        if (state == 0U) {
            uint32_t expected = 0U;
            if (__atomic_compare_exchange_n(&once->state, &expected, 1U, false,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                function();
                __atomic_store_n(&once->state, 2U, __ATOMIC_RELEASE);
                return futex_wake(&once->state, UINT32_MAX);
            }
            continue;
        }
        if (state != 1U) return EINVAL;
        if (futex_wait(&once->state, 1U, OS_WAIT_INFINITE) != 0) continue;
    }
}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    if (key == 0) return pthread_invalid();
    key_lock();
    for (pthread_key_t index = 0U; index < PTHREAD_KEYS_MAX; ++index) {
        uint64_t bit = 1ULL << index;
        if ((__atomic_load_n(&g_key_bitmap, __ATOMIC_RELAXED) & bit) == 0U) {
            uint32_t generation =
                __atomic_load_n(&g_key_generation[index], __ATOMIC_RELAXED) + 1U;
            /* 空闲键使用偶数令牌；下一个奇数令牌非零。 */
            g_key_destructors[index] = destructor;
            __atomic_fetch_or(&g_key_bitmap, bit, __ATOMIC_RELEASE);
            __atomic_store_n(&g_key_generation[index], generation, __ATOMIC_RELEASE);
            *key = index;
            key_unlock();
            return 0;
        }
    }
    key_unlock();
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key) {
    key_lock();
    if (!key_is_valid_locked(key)) {
        key_unlock();
        return EINVAL;
    }
    uint32_t generation = __atomic_load_n(&g_key_generation[key], __ATOMIC_RELAXED);
    __atomic_store_n(&g_key_generation[key], generation + 1U, __ATOMIC_RELEASE);
    __atomic_fetch_and(&g_key_bitmap, ~(1ULL << key), __ATOMIC_RELEASE);
    g_key_destructors[key] = 0;
    /* 初始线程通常使用 g_main_thread.specific。清除两种存储形式，以便删除并重用键时
     * 不会暴露先前的值。 */
    __atomic_store_n(&g_main_specific[key], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_main_specific_generation[key], 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&g_main_thread.specific[key], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_main_thread.specific_generation[key], 0U, __ATOMIC_RELEASE);
    key_unlock();
    return 0;
}

/* 键的分配和删除会串行化，但值属于调用线程。不要在所有 CPU 上串行化每次 TLS 访问。
 * 在访问槽位前后检查代数，可拒绝已删除或重用的键；原子元数据访问也能明确定义并发
 * 删除时的行为。 */
static bool key_generation_snapshot(pthread_key_t key, uint32_t *generation) {
    if (key >= PTHREAD_KEYS_MAX) return false;
    *generation = __atomic_load_n(&g_key_generation[key], __ATOMIC_ACQUIRE);
    return (*generation & 1U) != 0U;
}

static bool key_generation_matches(pthread_key_t key, uint32_t generation) {
    uint32_t current;
    return key_generation_snapshot(key, &current) && current == generation;
}

void *pthread_getspecific(pthread_key_t key) {
    pthread_t thread;
    void **values;
    uint32_t *generations;
    void *value = 0;
    uint32_t generation;
    if (!key_generation_snapshot(key, &generation)) return 0;
    thread = pthread_self();
    values = thread == 0 ? g_main_specific : thread->specific;
    generations = thread == 0 ? g_main_specific_generation :
                                thread->specific_generation;
    if (__atomic_load_n(&generations[key], __ATOMIC_ACQUIRE) == generation) {
        value = __atomic_load_n(&values[key], __ATOMIC_RELAXED);
    }
    return key_generation_matches(key, generation) ? value : 0;
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    pthread_t thread;
    void **values;
    uint32_t *generations;
    uint32_t generation;
    if (!key_generation_snapshot(key, &generation)) return EINVAL;
    thread = pthread_self();
    values = thread == 0 ? g_main_specific : thread->specific;
    generations = thread == 0 ? g_main_specific_generation :
                                thread->specific_generation;
    __atomic_store_n(&values[key], (void *)value, __ATOMIC_RELAXED);
    __atomic_store_n(&generations[key], value == 0 ? 0U : generation,
                     __ATOMIC_RELEASE);
    return key_generation_matches(key, generation) ? 0 : EINVAL;
}

int pthread_rwlockattr_init(pthread_rwlockattr_t *attribute) {
    if (attribute == 0) return pthread_invalid();
    attribute->process_shared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_rwlockattr_destroy(pthread_rwlockattr_t *attribute) {
    return attribute == 0 ? pthread_invalid() : 0;
}

int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *attribute, int shared) {
    if (attribute == 0) return pthread_invalid();
    if (shared != PTHREAD_PROCESS_PRIVATE && shared != PTHREAD_PROCESS_SHARED) {
        return EINVAL;
    }
    attribute->process_shared = shared;
    return 0;
}

int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *attribute,
                                  int *shared) {
    if (attribute == 0 || shared == 0) return pthread_invalid();
    *shared = attribute->process_shared;
    return 0;
}

int pthread_rwlock_init(pthread_rwlock_t *lock,
                        const pthread_rwlockattr_t *attribute) {
    if (lock == 0 || (attribute != 0 &&
        attribute->process_shared != PTHREAD_PROCESS_PRIVATE &&
        attribute->process_shared != PTHREAD_PROCESS_SHARED)) return EINVAL;
    if (__atomic_load_n(&lock->state, __ATOMIC_ACQUIRE) != 0U) return EBUSY;
    __atomic_store_n(&lock->state, 0U, __ATOMIC_RELAXED);
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *lock) {
    if (lock == 0) return pthread_invalid();
    return __atomic_load_n(&lock->state, __ATOMIC_ACQUIRE) == 0U ? 0 : EBUSY;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *lock) {
    uint32_t state;
    if (lock == 0) return pthread_invalid();
    state = __atomic_load_n(&lock->state, __ATOMIC_ACQUIRE);
    while ((state & PTHREAD_RWLOCK_WRITER) == 0U) {
        if ((state & PTHREAD_RWLOCK_READERS) == PTHREAD_RWLOCK_READERS) return EAGAIN;
        if (__atomic_compare_exchange_n(&lock->state, &state, state + 1U,
                                        false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) return 0;
    }
    return EBUSY;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *lock) {
    int error;
    if (lock == 0) return pthread_invalid();
    for (;;) {
        error = pthread_rwlock_tryrdlock(lock);
        if (error == 0) return 0;
        if (error != EBUSY && error != EAGAIN) return error;
        uint32_t state = __atomic_load_n(&lock->state, __ATOMIC_ACQUIRE);
        if (futex_wait(&lock->state, state, OS_WAIT_INFINITE) != 0) continue;
    }
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *lock) {
    uint32_t expected = 0U;
    if (lock == 0) return pthread_invalid();
    return __atomic_compare_exchange_n(&lock->state, &expected,
                                       PTHREAD_RWLOCK_WRITER, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ?
           0 : EBUSY;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *lock) {
    if (lock == 0) return pthread_invalid();
    for (;;) {
        int error = pthread_rwlock_trywrlock(lock);
        if (error == 0) return 0;
        if (futex_wait(&lock->state,
                       __atomic_load_n(&lock->state, __ATOMIC_ACQUIRE),
                       OS_WAIT_INFINITE) != 0) continue;
    }
}

int pthread_rwlock_unlock(pthread_rwlock_t *lock) {
    uint32_t state;
    if (lock == 0) return pthread_invalid();
    state = __atomic_load_n(&lock->state, __ATOMIC_ACQUIRE);
    if (state == PTHREAD_RWLOCK_WRITER) {
        uint32_t expected = PTHREAD_RWLOCK_WRITER;
        if (!__atomic_compare_exchange_n(&lock->state, &expected, 0U, false,
                                         __ATOMIC_RELEASE,
                                         __ATOMIC_RELAXED)) return EPERM;
        return futex_wake(&lock->state, UINT32_MAX);
    }
    for (;;) {
        uint32_t readers = state & PTHREAD_RWLOCK_READERS;
        if (state & PTHREAD_RWLOCK_WRITER || readers == 0U) return EPERM;
        if (__atomic_compare_exchange_n(&lock->state, &state, state - 1U,
                                        false, __ATOMIC_RELEASE,
                                        __ATOMIC_ACQUIRE)) {
            return readers == 1U ? futex_wake(&lock->state, UINT32_MAX) : 0;
        }
    }
}

int pthread_barrierattr_init(pthread_barrierattr_t *attribute) {
    if (attribute == 0) return pthread_invalid();
    attribute->process_shared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_barrierattr_destroy(pthread_barrierattr_t *attribute) {
    return attribute == 0 ? pthread_invalid() : 0;
}

int pthread_barrierattr_setpshared(pthread_barrierattr_t *attribute, int shared) {
    if (attribute == 0) return pthread_invalid();
    if (shared != PTHREAD_PROCESS_PRIVATE && shared != PTHREAD_PROCESS_SHARED) {
        return EINVAL;
    }
    attribute->process_shared = shared;
    return 0;
}

int pthread_barrierattr_getpshared(const pthread_barrierattr_t *attribute,
                                   int *shared) {
    if (attribute == 0 || shared == 0) return pthread_invalid();
    *shared = attribute->process_shared;
    return 0;
}

int pthread_barrier_init(pthread_barrier_t *barrier,
                         const pthread_barrierattr_t *attribute,
                         unsigned int count) {
    if (barrier == 0 || count == 0U || (attribute != 0 &&
        attribute->process_shared != PTHREAD_PROCESS_PRIVATE &&
        attribute->process_shared != PTHREAD_PROCESS_SHARED)) return EINVAL;
    barrier->threshold = count;
    __atomic_store_n(&barrier->arrived, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&barrier->generation, 0U, __ATOMIC_RELAXED);
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *barrier) {
    if (barrier == 0) return pthread_invalid();
    return __atomic_load_n(&barrier->arrived, __ATOMIC_ACQUIRE) == 0U ? 0 : EBUSY;
}

int pthread_barrier_wait(pthread_barrier_t *barrier) {
    uint32_t generation;
    uint32_t arrived;
    if (barrier == 0 || barrier->threshold == 0U) return pthread_invalid();
    generation = __atomic_load_n(&barrier->generation, __ATOMIC_ACQUIRE);
    arrived = __atomic_add_fetch(&barrier->arrived, 1U, __ATOMIC_ACQ_REL);
    if (arrived == barrier->threshold) {
        __atomic_store_n(&barrier->arrived, 0U, __ATOMIC_RELEASE);
        __atomic_add_fetch(&barrier->generation, 1U, __ATOMIC_RELEASE);
        (void)futex_wake(&barrier->generation, UINT32_MAX);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (__atomic_load_n(&barrier->generation, __ATOMIC_ACQUIRE) == generation) {
        (void)futex_wait(&barrier->generation, generation, OS_WAIT_INFINITE);
    }
    return 0;
}

int pthread_spin_init(pthread_spinlock_t *lock, int shared) {
    if (lock == 0) return pthread_invalid();
    if (shared != PTHREAD_PROCESS_PRIVATE && shared != PTHREAD_PROCESS_SHARED) {
        return EINVAL;
    }
    __atomic_store_n(&lock->state, 0U, __ATOMIC_RELAXED);
    return 0;
}

int pthread_spin_destroy(pthread_spinlock_t *lock) {
    return lock == 0 ? pthread_invalid() : 0;
}

int pthread_spin_trylock(pthread_spinlock_t *lock) {
    uint32_t expected = 0U;
    if (lock == 0) return pthread_invalid();
    return __atomic_compare_exchange_n(&lock->state, &expected, 1U, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ?
           0 : EBUSY;
}

int pthread_spin_lock(pthread_spinlock_t *lock) {
    if (lock == 0) return pthread_invalid();
    while (pthread_spin_trylock(lock) != 0) __asm__ volatile("pause");
    return 0;
}

int pthread_spin_unlock(pthread_spinlock_t *lock) {
    if (lock == 0) return pthread_invalid();
    if (__atomic_exchange_n(&lock->state, 0U, __ATOMIC_RELEASE) == 0U) return EPERM;
    return 0;
}

static void pthread_thread_start(void *raw_context) {
    pthread_start_context_t *context = (pthread_start_context_t *)raw_context;
    void *result = context->start_routine(context->argument);
    pthread_exit(result);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attribute,
                   void *(*start_routine)(void *), void *argument) {
    pthread_attr_t defaults;
    pthread_t record;
    pthread_start_context_t *context;
    void *stack;
    void *mapping_base;
    size_t stack_size;
    size_t guard_size;
    size_t mapped_size;
    size_t mapping_size;
    size_t stack_extent;
    uintptr_t stack_top;
    os_thread_create_t arguments;
    os_thread_stack_t stack_arguments;
    os_handle_t handle = OS_INVALID_HANDLE;
    int64_t status;
    bool linux_personality;

    detached_reap();
    if (thread == 0 || start_routine == 0) return pthread_invalid();
    if (attribute == 0) {
        if (pthread_attr_init(&defaults) != 0) return EINVAL;
        attribute = &defaults;
    }
    if ((attribute->detach_state != PTHREAD_CREATE_JOINABLE &&
         attribute->detach_state != PTHREAD_CREATE_DETACHED) ||
        attribute->stack_size < PTHREAD_STACK_MIN) return EINVAL;
    stack_size = attribute->stack_size;
    guard_size = attribute->guard_size;
    mapped_size = page_round(stack_size);
    if (mapped_size == 0U) return EOVERFLOW;
    if (attribute->stack_address == 0) {
        if (guard_size > SIZE_MAX - mapped_size) return EOVERFLOW;
        mapping_size = guard_size + mapped_size;
        if (mapping_size == 0U) return EOVERFLOW;
    } else {
        mapping_size = mapped_size;
    }
    if (attribute->stack_address != 0) {
        stack = attribute->stack_address;
        mapping_base = stack;
        stack_extent = stack_size;
    } else {
        mapping_base = mmap(0, mapping_size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
        if (mapping_base == MAP_FAILED) return errno != 0 ? errno : ENOMEM;
        if (guard_size != 0U &&
            mprotect(mapping_base, guard_size, PROT_NONE) < 0) {
            (void)munmap(mapping_base, mapping_size);
            return errno != 0 ? errno : EIO;
        }
        if ((uintptr_t)mapping_base > UINTPTR_MAX - guard_size) {
            (void)munmap(mapping_base, mapping_size);
            return EOVERFLOW;
        }
        stack = (void *)((uintptr_t)mapping_base + guard_size);
        stack_extent = mapped_size;
    }
    if ((uintptr_t)stack > UINTPTR_MAX - stack_extent) {
        if (attribute->stack_address == 0) (void)munmap(mapping_base, mapping_size);
        return EOVERFLOW;
    }
    /* 新调度的入口没有 CALL 压入的返回地址。将其起始地址设为模 16 余 8，使首个 C
     * 函数满足 SysV ABI 入口对齐要求（exec 入口本身仍按 16 字节对齐）。 */
    stack_top = (((uintptr_t)stack + stack_extent) & ~(uintptr_t)0x0fU) -
                sizeof(uint64_t);
    if (stack_top <= (uintptr_t)stack) {
        if (attribute->stack_address == 0) (void)munmap(mapping_base, mapping_size);
        return EINVAL;
    }
    record = (pthread_t)malloc(sizeof(*record));
    context = (pthread_start_context_t *)malloc(sizeof(*context));
    if (record == 0 || context == 0) {
        free(record);
        free(context);
        if (attribute->stack_address == 0) (void)munmap(mapping_base, mapping_size);
        return ENOMEM;
    }
    memset(record, 0, sizeof(*record));
    record->stack_address = stack;
    record->stack_size = stack_extent;
    record->owns_stack = attribute->stack_address == 0;
    record->lifecycle_lock = 0U;
    record->joined = false;
    record->detached = attribute->detach_state == PTHREAD_CREATE_DETACHED;
    record->exited = false;
    record->listed = false;
    record->cleanup = false;
    record->handle_ready = false;
    record->detached_next = 0;
    record->context = context;
    context->thread = record;
    context->start_routine = start_routine;
    context->argument = argument;
    context->error_number = 0;
    linux_personality = pthread_linux_personality();
    if (linux_personality && !pthread_prepare_tls(record)) {
        int error = errno != 0 ? errno : EIO;
        pthread_release_tls(record);
        free(context);
        free(record);
        if (attribute->stack_address == 0) (void)munmap(mapping_base, mapping_size);
        return error;
    }
    memset(&arguments, 0, sizeof(arguments));
    arguments.hdr.size = sizeof(arguments);
    arguments.hdr.version = OS_SYSCALL_ABI_VERSION;
    arguments.entry = (uint64_t)(uintptr_t)pthread_thread_start;
    arguments.stack_top = (uint64_t)stack_top;
    arguments.fs_base = record->tls_fs_base != 0U ? record->tls_fs_base :
                        (uint64_t)(uintptr_t)record;
    arguments.argument = (uint64_t)(uintptr_t)context;
    memset(&stack_arguments, 0, sizeof(stack_arguments));
    if (record->owns_stack) {
        stack_arguments.hdr.size = sizeof(stack_arguments);
        stack_arguments.hdr.version = OS_SYSCALL_ABI_VERSION;
        stack_arguments.base = (uint64_t)(uintptr_t)mapping_base;
        stack_arguments.size = (uint64_t)mapping_size;
        stack_arguments.flags = OS_THREAD_STACK_OWNED;
    }
    status = __libc_native_syscall6(OS_SYS_THREAD_CREATE, OS_INVALID_HANDLE,
                             (uint64_t)(uintptr_t)&arguments,
                             (uint64_t)(uintptr_t)&handle,
                             record->owns_stack ?
                                 (uint64_t)(uintptr_t)&stack_arguments : 0U,
                             0U, 0U);
    if (status < 0) {
        int error = pthread_status_error(status);
        pthread_release_tls(record);
        free(context);
        free(record);
        if (attribute->stack_address == 0) (void)munmap(mapping_base, mapping_size);
        return error;
    }
    record->handle = handle;
    record->handle_ready = true;
    if (record->detached) detached_list_add(record);
    *thread = record;
    return 0;
}

int pthread_join(pthread_t thread, void **result) {
    os_wait_result_t wait_result = {0};
    int64_t status;
    int error;
    if (thread == 0) {
        return pthread_invalid();
    }
    if (thread == pthread_self()) return EDEADLK;

    /* 等待前先声明 join 所有权，避免第一个等待者完成后，第二个 join 调用方释放同一记录。 */
    thread_lifecycle_lock(thread);
    if (thread->joined || thread->detached || thread->cleanup) {
        thread_lifecycle_unlock(thread);
        return pthread_invalid();
    }
    thread->joined = true;
    thread_lifecycle_unlock(thread);

    status = __libc_native_syscall6(OS_SYS_WAIT_ONE, thread->handle,
                             OS_WAIT_INFINITE,
                             (uint64_t)(uintptr_t)&wait_result,
                             0U, 0U, 0U);
    error = pthread_status_error(status);
    if (error != 0) {
        thread_lifecycle_lock(thread);
        thread->joined = false;
        thread_lifecycle_unlock(thread);
        return error;
    }
    if (result != 0) *result = (void *)(uintptr_t)wait_result.value;
    (void)__libc_native_syscall6(OS_SYS_HANDLE_CLOSE, thread->handle,
                          0U, 0U, 0U, 0U, 0U);
    /* 所有权属于线程的栈会在上下文切换后由内核回收。 */
    pthread_release_tls(thread);
    free(thread->context);
    free(thread);
    detached_reap();
    return 0;
}

int pthread_detach(pthread_t thread) {
    bool handle_ready;
    if (thread == 0) {
        return pthread_invalid();
    }

    thread_lifecycle_lock(thread);
    if (thread->joined || thread->detached || thread->cleanup) {
        thread_lifecycle_unlock(thread);
        return pthread_invalid();
    }
    thread->detached = true;
    handle_ready = thread->handle_ready;
    thread_lifecycle_unlock(thread);

    if (handle_ready) detached_list_add(thread);
    detached_reap();
    return 0;
}

void pthread_exit(void *result) {
    pthread_t thread = pthread_self();

    if (thread != 0) {
        run_key_destructors(thread);
        /*
         * 在线程 join 调用方（或延迟的分离线程回收器）观察到 WAIT_ONE 完成前，线程记录
         * 及其 FS 上下文必须保持有效。尤其是在 THREAD_EXIT 切换到其他上下文前，不得取消
         * 映射或释放当前用户栈及 FS 对象。
         */
        __atomic_store_n(&thread->exited, true, __ATOMIC_RELEASE);
    }
    (void)__libc_native_syscall6(OS_SYS_THREAD_EXIT,
                          (uint64_t)(uintptr_t)result,
                          0U, 0U, 0U, 0U, 0U);
    for (;;) __asm__ volatile("pause");
}

pthread_t pthread_self(void) {
    uint64_t marker = 0U;
    pthread_t thread = 0;
    int64_t fs_status;
    __asm__ volatile("movq %%fs:24, %0" : "=r"(marker));
    if (marker == LIBC_TLS_MAGIC) {
        __asm__ volatile("movq %%fs:32, %0" : "=r"(thread));
    return thread;
}
    fs_status = __libc_native_syscall6(OS_SYS_THREAD_CONTEXT,
                                OS_THREAD_CONTEXT_GET_FS,
                                0U, 0U, 0U, 0U, 0U);
    return fs_status <= 0 ? 0 : (pthread_t)(uintptr_t)fs_status;
}

int pthread_equal(pthread_t left, pthread_t right) {
    return left == right;
}

int pthread_kill(pthread_t thread, int signal_number) {
    if (thread == 0 || signal_number < 0 ||
        signal_number > (int)OS_SIGNAL_COUNT) {
        return EINVAL;
    }
    return pthread_status_error(__libc_native_syscall6(
        OS_SYS_THREAD_SIGNAL, thread->handle, (uint64_t)(uint32_t)signal_number,
        0U, 0U, 0U, 0U));
}

int pthread_setname_np(pthread_t thread, const char *name) {
    size_t length;
    if (thread == 0 || name == 0) return EINVAL;
    length = strlen(name);
    if (length >= sizeof(thread->name)) return ERANGE;
    memcpy(thread->name, name, length + 1U);
    return 0;
}

int pthread_getattr_np(pthread_t thread, pthread_attr_t *attribute) {
    if (thread == 0 || attribute == 0) return pthread_invalid();
    if (pthread_attr_init(attribute) != 0) return pthread_invalid();
    if (thread == &g_main_thread) {
        /* 此处必须与 exec.c 中的初始栈映射保持一致。 */
        attribute->stack_address = (void *)(uintptr_t)
            OS_LINUX_USER_STACK_BASE;
        attribute->stack_size = (size_t)OS_LINUX_USER_STACK_SIZE;
    } else {
        attribute->stack_address = thread->stack_address;
        attribute->stack_size = thread->stack_size;
    }
    return 0;
}

int pthread_getname_np(pthread_t thread, char *name, size_t length) {
    if (thread == 0 || name == 0 || length == 0U) return ERANGE;
    if (thread->name[0] == '\0') {
        name[0] = '\0';
        return 0;
    }
    if (strlen(thread->name) + 1U > length) return ERANGE;
    memcpy(name, thread->name, strlen(thread->name) + 1U);
    return 0;
}

int pthread_getschedparam(pthread_t thread, int *policy,
                          struct sched_param *parameter) {
    if (thread == 0 || policy == 0 || parameter == 0) return EINVAL;
    *policy = SCHED_OTHER;
    parameter->sched_priority = 0;
    return 0;
}

int pthread_setschedparam(pthread_t thread, int policy,
                          const struct sched_param *parameter) {
    if (thread == 0 || parameter == 0) return EINVAL;
    if (policy != SCHED_OTHER || parameter->sched_priority != 0) {
        return ENOTSUP;
    }
    return 0;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *old_set) {
    int result = sigprocmask(how, set, old_set);
    return result == 0 ? 0 : errno;
}

int pthread_atfork(void (*prepare)(void), void (*parent)(void),
                   void (*child)(void)) {
    if (prepare == 0 && parent == 0 && child == 0) return EINVAL;
    atfork_lock();
    if (g_atfork_count >= PTHREAD_ATFORK_MAX) {
        atfork_unlock();
        return ENOMEM;
    }
    g_atfork_entries[g_atfork_count].prepare = prepare;
    g_atfork_entries[g_atfork_count].parent = parent;
    g_atfork_entries[g_atfork_count].child = child;
    ++g_atfork_count;
    atfork_unlock();
    return 0;
}

void __libc_atfork_prepare(void) {
    pthread_atfork_entry_t entries[PTHREAD_ATFORK_MAX];
    unsigned int count = atfork_snapshot(entries);
    while (count != 0U) {
        void (*prepare)(void) = entries[count - 1U].prepare;
        if (prepare != 0) prepare();
        --count;
    }
}

void __libc_atfork_parent(void) {
    pthread_atfork_entry_t entries[PTHREAD_ATFORK_MAX];
    unsigned int count = atfork_snapshot(entries);
    for (unsigned int index = 0U; index < count; ++index) {
        void (*parent)(void) = entries[index].parent;
        if (parent != 0) parent();
    }
}

void __libc_atfork_child(void) {
    pthread_atfork_entry_t entries[PTHREAD_ATFORK_MAX];
    unsigned int count = atfork_snapshot(entries);
    for (unsigned int index = 0U; index < count; ++index) {
        void (*child)(void) = entries[index].child;
        if (child != 0) child();
    }
}
